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
		Resend,     // timed out, attempts remain - send the transfer again
		Hold,       // timed out, attempts exhausted, BUT the peer link is still there:
		            // keep the object in custody and stop acting on it. Never reclaim.
		Reclaim,    // the peer link is gone - take the object back
	};

	struct CustodyConfig {
		// Ticks to wait for an ack before resending. 0 disables retry entirely and
		// restores the original behaviour, so an experiment can compare the two.
		int retryTicks = 30;
		// Total sends allowed, including the first. Past this the action becomes Hold,
		// not Reclaim - see the comment on DecideCustody below.
		int maxAttempts = 3;
	};

	// RECLAIM IS GATED ON EVIDENCE THAT THE PEER IS GONE, NEVER ON ELAPSED TIME.
	//
	// The first version of this function reclaimed once `attempts >= maxAttempts`
	// timeouts had passed. A timeout cannot distinguish "the receiver never got it"
	// from "the receiver got it and is slow", and under overload it is systematically
	// the latter: measurement (docs/superpowers/results/2026-08-20-B-custody.md) showed
	// the sender reclaiming while the original transfer was still landing on the
	// receiver, so BOTH servers ended up owning the object. conservation_delta went
	// from -4..-8 (loss, which custody was built to fix) to +100/+113 at 8k objects,
	// +627 at 4k, and +13,532 with rebalancing - over 3x the population. Duplicate
	// ownership is strictly worse than the loss it replaced: two integrators diverge
	// and nothing downstream can tell which copy is real.
	//
	// `peerLinkGone` is the only admissible evidence: a resend the network layer could
	// not even attempt, because there is no peer link to that server at all. Note this
	// is NARROWER than "SendPacketToServer returned false" - ENet also refuses a send
	// when the peer's outgoing reliable queue is full, which is backpressure under
	// overload, i.e. exactly the condition custody must not mistake for death. The
	// caller distinguishes the two (DistributedGameServerManager::HasPeerLink) and only
	// a genuinely absent link reaches here. Measured: treating a queue refusal as death
	// still produced reclaims, and they still duplicated (fix3-E7 r2, +124).
	//
	// A delivered resend proves the peer is alive, so custody keeps waiting.
	//
	// ACCEPTED TRADE-OFF: a peer that is alive but permanently wedged - accepting
	// packets, never acking - will never return the object, and custody holds it
	// forever. That is a STALL, permanently visible in the hoCustody counter and in
	// the conservation total, rather than a silent corruption that no counter can
	// see. A visible stall is strictly better than duplicate ownership.
	//
	// Hold also stops RESENDING, not just reclaiming. Resends are only safe while the
	// receiver still holds the object: it acks the duplicate and custody discharges.
	// Once the receiver has handed the object onward - which on a moving workload takes
	// tens of ticks, far less than the retry deadline - a late resend RE-INSTALLS an
	// object that now lives somewhere else, which is duplication by a second route.
	// An unbounded-resend variant of this function was measured doing exactly that:
	// 4,117 resends against 199 transfers, hoRecv 846 against hoSent 199, and
	// conservation_delta +1,463. maxAttempts is what bounds that exposure, so it is
	// still enforced - it just ends in Hold instead of Reclaim.
	inline CustodyAction DecideCustody(uint64_t currentTick, uint64_t lastSentTick,
		int attempts, const CustodyConfig& config, bool peerLinkGone) {
		if (config.retryTicks <= 0) {
			// Custody disabled entirely (the pre-custody comparison build). No resends
			// happen, so there is never a delivery result to judge either.
			return CustodyAction::Wait;
		}
		// Checked BEFORE the deadline: a missing peer link is evidence, not a timeout,
		// so there is nothing to wait for. Deferring the reclaim to the next deadline
		// would leave the object nowhere for another full retry window.
		if (peerLinkGone) {
			return CustodyAction::Reclaim;
		}
		// The tick counter can sit below lastSentTick after a repartition. Unsigned
		// subtraction would wrap to an enormous elapsed value and act instantly.
		if (currentTick < lastSentTick) {
			return CustodyAction::Wait;
		}
		if ((currentTick - lastSentTick) < static_cast<uint64_t>(config.retryTicks)) {
			return CustodyAction::Wait;
		}
		if (attempts >= config.maxAttempts) {
			// The peer link is still there (or peerLinkGone would have returned above),
			// so the transfer is on its way or already installed. Hold: the caller keeps
			// the custody record and stops acting on it - no further resend, and above
			// all no apply back to itself.
			return CustodyAction::Hold;
		}
		return CustodyAction::Resend;
	}

	// --- Duplicate-arrival guard ---------------------------------------------------
	//
	// A resend (CustodyAction::Resend) puts a SECOND copy of an already-delivered
	// transfer packet on the wire. That is by design - it is what discharges custody
	// when an ack was lost - but it means the receiver's apply path can be entered
	// twice for the same object, and that path was NOT idempotent below the pool
	// lookup: it re-pushed the object into the simulated-object list every time, so
	// the world population grew by one per redundant arrival and the object's control
	// forces were applied once per duplicate entry, every tick. Measured at 8,000
	// objects / 2 servers, conservation_delta went +793 / +856 / +34 against a -4..-8
	// baseline.
	//
	// This predicate decides whether an arriving transfer is a redundant copy of one
	// this server has ALREADY installed, in which case the correct response is to
	// accept and acknowledge it (so the sender's custody discharges) while doing
	// nothing else at all.
	//
	//  - objectPresent: the receiver's pool holds a live (non-null) entry for the id.
	//    A handoff already passed onward ERASES the pool entry, and a destroyed object
	//    NULLS it, so neither looks present here - both must keep their existing
	//    handling.
	//  - networkActive: the entry is not a deactivated husk mid-installation. Only an
	//    object that has completed the shared registration path is active, so this is
	//    what distinguishes "already simulating it" from "known but not installed".
	//  - isHaloShadow: a shadow is a read-only copy of a NEIGHBOUR's object. It is
	//    present in the pool but is emphatically not owned here, and its handoff must
	//    still run the promotion path - so a shadow is never a duplicate.
	//  - isReclaim: a reclaim is the sender re-applying its own transfer to itself
	//    after the peer link died. It is exempt because its whole purpose is to
	//    re-install an object; suppressing it would strand the object nowhere.
	inline bool IsDuplicateHandoffArrival(bool isReclaim, bool objectPresent,
		bool networkActive, bool isHaloShadow) {
		if (isReclaim) {
			return false;
		}
		if (!objectPresent) {
			return false;
		}
		if (isHaloShadow) {
			return false;
		}
		return networkActive;
	}

	// --- Redundant-reclaim guard ---------------------------------------------------
	//
	// A reclaim is exempt from IsDuplicateHandoffArrival above because its purpose is
	// to re-install an object that exists nowhere. But a reclaim can fire while the
	// object is STILL HERE, because the release and the reclaim are on two different
	// clocks:
	//
	//   - the release is scheduled on a SIMULATION tick (senderTick + handoffLookahead;
	//     at --handoff-lookahead 300 that is 2.5s of simulated time at 120 Hz), and
	//   - the reclaim deadline is WALL-CLOCK (a resend with no peer link, floored at
	//     the 1s cold-connection floor).
	//
	// So a peer that dies immediately after a transfer is sent can be detected, and the
	// object reclaimed, BEFORE FlushScheduledReleases has released it at all. The
	// sender then re-applies its own packet to an object it never let go of, and the
	// non-idempotent shared path below the pool lookup runs a second time - exactly the
	// duplication IsDuplicateHandoffArrival exists to prevent, reintroduced through the
	// isReclaim exemption.
	//
	// Reclaiming something never actually released is a no-op ON THE OBJECT: it is
	// present, network-active and already ours. The caller still has work to do -
	// discharge custody and restore the ownership bookkeeping the reclaim branch
	// cleared - but it must not re-register the object.
	//
	// A halo shadow is excluded for the same reason as in the duplicate guard: a shadow
	// is a neighbour's read-only copy, not our retained object, so its reclaim must run
	// the full promotion path.
	inline bool IsRedundantReclaim(bool isReclaim, bool objectPresent,
		bool networkActive, bool isHaloShadow) {
		if (!isReclaim) {
			return false;
		}
		if (!objectPresent || isHaloShadow) {
			return false;
		}
		return networkActive;
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

	// The identity of a transfer this server has already accepted for one object.
	// (senderServerID, senderTick) rather than a new sequence field: both are already on
	// StartSimulatingObjectPacket, so this costs no wire-format change, and custody holds
	// the packet verbatim so a resend reproduces both exactly.
	struct AcceptedTransfer {
		int       senderServerID = -1;
		long long senderTick = -1;
	};

	// True when an arriving transfer is a resend of one already accepted for this object.
	//
	// IsDuplicateHandoffArrival above cannot answer this. It tests object STATE, and a
	// resend that arrives after the object was handed ONWARD finds it present-but-inactive
	// - pool entries are never erased, because ids are never recycled - which is
	// indistinguishable from a genuine new handoff of an object returning here. So the
	// arrival fell through and was re-installed while the server it had been handed to
	// still owned it: two owners, one object, integrated twice. Backlog item 15; one
	// repeat of three ended holding 1,303 more objects than the world contains.
	//
	// The test is "not strictly newer than what we accepted", not "equal to it", because
	// delivery can reorder: a resend may arrive AFTER a later transfer of the same object
	// was already accepted, and an equality test would let that one through.
	//
	// A reclaim is exempt, for the same reason IsDuplicateHandoffArrival exempts it: a
	// reclaim is the sender re-applying its own packet after the peer link died, so it
	// carries the original identity by construction and would match every time - and
	// dropping it strands the object nowhere, the loss custody exists to prevent.
	// IsRedundantReclaim is what handles the reclaim case.
	inline bool IsResendOfAcceptedTransfer(bool isReclaim, bool hasAcceptedRecord,
		const AcceptedTransfer& accepted, int arrivingSenderID, long long arrivingSenderTick) {
		if (isReclaim) {
			return false;
		}
		if (!hasAcceptedRecord) {
			return false;
		}
		if (arrivingSenderID != accepted.senderServerID) {
			return false;
		}
		return arrivingSenderTick <= accepted.senderTick;
	}
}
