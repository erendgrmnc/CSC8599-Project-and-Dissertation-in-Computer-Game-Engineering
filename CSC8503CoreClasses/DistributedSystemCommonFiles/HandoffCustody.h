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
}
