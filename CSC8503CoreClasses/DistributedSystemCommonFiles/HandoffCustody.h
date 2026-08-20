#pragma once
#include <cstdint>

// When a server hands an object to a neighbour it tears its own copy down - the pool
// entry is ERASED, not nulled - so if the receiver never installs the object it exists
// nowhere. Transfers and acks both go over reliable ENet, so ordinary packet loss is
// already covered; the exposures are a receiver that REFUSES the object
// (StartHandlingObject returning false, which acks nothing) and a peer that dies
// mid-transfer.
//
// The sender therefore keeps the transfer packet until the receiver acknowledges it.
// This decides what to do with one still-unacknowledged transfer. Acknowledged ones
// are erased on arrival of the ack and never reach here.
//
// Deliberately NOT a decision about when to release the object. Releasing late would
// mean the sender kept simulating an object the receiver had already started
// simulating, turning an ownership gap into an ownership OVERLAP with two integrators
// diverging - strictly worse than the sub-millisecond freeze a gap causes. Release
// timing is unchanged; only the retry obligation is new.
namespace NCL::Distributed {

	enum class CustodyAction {
		Wait,       // still within the retry window
		Resend,     // timed out, attempts remain
		Reclaim,    // timed out, attempts exhausted - take the object back
	};

	struct CustodyConfig {
		// Ticks to wait for an ack before resending. 0 disables retry entirely and
		// restores the original behaviour, so an experiment can compare the two.
		int retryTicks = 30;
		// Total sends allowed, including the first. 1 means reclaim on first timeout.
		int maxAttempts = 3;
	};

	inline CustodyAction DecideCustody(uint64_t currentTick, uint64_t lastSentTick,
		int attempts, const CustodyConfig& config) {
		if (config.retryTicks <= 0) {
			return CustodyAction::Wait;
		}
		// The tick counter can sit below lastSentTick after a repartition. Unsigned
		// subtraction would wrap to an enormous elapsed value and reclaim instantly.
		if (currentTick < lastSentTick) {
			return CustodyAction::Wait;
		}
		if ((currentTick - lastSentTick) < static_cast<uint64_t>(config.retryTicks)) {
			return CustodyAction::Wait;
		}
		if (attempts >= config.maxAttempts) {
			return CustodyAction::Reclaim;
		}
		return CustodyAction::Resend;
	}

	// --- Wall-clock retry budget derivation ---------------------------------------
	//
	// DecideCustody above is deliberately unit-agnostic: it just compares two
	// uint64_t "tick" values, and does not care what unit they are actually counted
	// in. What VALUE those ticks hold is decided by the two functions below, pulled
	// out here (rather than left inline at the ServerWorldManager call site) so the
	// same architectural rule applies to them as to DecideCustody itself: custody
	// decision logic lives in one pure, unit-tested place.
	//
	// Background (Task 6a): the retry deadline used to be a count of the SENDER's
	// own mTickCounter. That is not a valid proxy for elapsed real time - a
	// lightly-loaded sender can tick in tens of microseconds while a receiver
	// legitimately needs hundreds of milliseconds to apply and acknowledge a large
	// simultaneous handoff burst - so it fired false resends/reclaims on a healthy
	// run. The deadline is now measured against NCL::MonotonicMicros() instead.

	// Converts an operator-facing "N ticks at the nominal rate" budget
	// (--handoff-retry-ticks) into real microseconds. nominalDtSeconds is the
	// substep duration (ServerWorldManager::GetFixedTimestepDt(), or a 120 Hz
	// fallback when unset); maxMicros is a hard ceiling so a very large configured
	// value cannot overflow when ScaleCustodyRetryMicros later multiplies it by a
	// batch size.
	inline int64_t CustodyTicksToMicros(int retryTicks, double nominalDtSeconds, int64_t maxMicros) {
		if (retryTicks < 0) {
			retryTicks = 0;
		}
		if (!(nominalDtSeconds > 0.0)) {
			nominalDtSeconds = 1.0 / 120.0;
		}
		if (maxMicros < 0) {
			maxMicros = 0;
		}
		double micros = static_cast<double>(retryTicks) * nominalDtSeconds * 1e6;
		if (micros > static_cast<double>(maxMicros)) {
			micros = static_cast<double>(maxMicros);
		}
		if (micros < 0.0) {
			micros = 0.0;
		}
		return static_cast<int64_t>(micros);
	}

	// Scales the configured retry budget (already in microseconds, from
	// CustodyTicksToMicros) by how many OTHER handoffs a receiver was sent in the
	// same tick as this one (batchSize): applying and acknowledging N handoffs
	// takes a receiver roughly N times as long as one, so a deadline sized for a
	// lone transfer fires while a receiver is still legitimately working through a
	// large, simultaneous batch.
	//
	// The result is a three-way MAX of configuredMicros, coldConnectionFloorMicros
	// and the capped scale-up, so none of the three can ever make the effective
	// deadline SHORTER than what the operator configured - scaling must only ever
	// be generous:
	//   - configuredMicros: a cap alone (min(scaled, capMicros)) can clamp BELOW a
	//     configuredMicros that already exceeds the cap on its own, silently
	//     shortening a deadline the operator deliberately widened. Folding
	//     configuredMicros into the max is what stops that.
	//   - coldConnectionFloorMicros: even a single, unbatched handoff (batchSize 1 -
	//     the normal case on `uniform`/`shuttle`, not just the bursty `headon`) can
	//     be the very first reliable traffic on a freshly-connected peer link, which
	//     was measured (Task 6a) to take ~571ms of wall-clock time before the first
	//     ack came back - comfortably more than the ~250ms a 30-tick nominal budget
	//     gives at 120 Hz. Batch scaling alone does not help a lone crossing, so this
	//     floor is applied unconditionally, independent of batch size.
	//   - min(configuredMicros * batchSize, capMicros): the batch-scaled budget,
	//     capped so a very large migration cannot defer detecting a genuinely dead
	//     peer indefinitely.
	//
	// EXCEPT when configuredMicros is 0. CustodyConfig::retryTicks documents 0 as
	// "disables retry entirely - restores the original (pre-custody) behaviour, so
	// an experiment can compare the two" - a load-bearing escape hatch, not a small
	// timeout. The floor and the cap both exist to stop a deadline from firing
	// PREMATURELY; "disabled" is not a premature timeout, it is no timeout, and
	// DecideCustody's own `config.retryTicks <= 0` check (above) is what implements
	// that. Folding a non-zero floor into the max here would make a configured 0
	// silently become the floor value and never reach that check, so 0 is handled
	// first and short-circuits past every other term.
	inline int64_t ScaleCustodyRetryMicros(int64_t configuredMicros, int batchSize,
		int64_t capMicros, int64_t coldConnectionFloorMicros) {
		if (configuredMicros <= 0) {
			return 0;
		}
		if (batchSize < 1) {
			batchSize = 1;
		}
		if (capMicros < 0) {
			capMicros = 0;
		}
		if (coldConnectionFloorMicros < 0) {
			coldConnectionFloorMicros = 0;
		}
		const int64_t scaled = configuredMicros * static_cast<int64_t>(batchSize);
		const int64_t boundedScaled = (scaled < capMicros) ? scaled : capMicros;
		int64_t result = (configuredMicros > boundedScaled) ? configuredMicros : boundedScaled;
		if (coldConnectionFloorMicros > result) {
			result = coldConnectionFloorMicros;
		}
		return result;
	}
}
