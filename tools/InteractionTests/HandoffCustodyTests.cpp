// The custody decision for an unacknowledged handoff.
//
// Acknowledged transfers are ERASED from mPendingTransfers, so they never reach this
// function - it only ever sees transfers still outstanding. That is what keeps it a
// pure function of (tick, lastSentTick, attempts, config, peerLinkGone) with no
// "acked" input.
//
// The last parameter is the whole point of the Task 6a fix: reclaim is gated on
// evidence that the peer is GONE (a resend with no peer link to send it down at all),
// never on elapsed time. Time-gated reclaim duplicated objects under load - it cannot
// tell "never arrived" from "arrived and the receiver is slow", and under overload it
// is systematically the latter.

#include "TestHarness.h"

#include "DistributedSystemCommonFiles/HandoffCustody.h"

using namespace NCL::Distributed;

TEST(CustodyWaitsBeforeTheRetryTimeout) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	// Sent on tick 100, now tick 129: 29 ticks elapsed, one short of the timeout.
	CHECK(DecideCustody(129, 100, 1, config, false) == CustodyAction::Wait);
}

TEST(CustodyResendsExactlyOnTheTimeoutTick) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	// Boundary: elapsed == retryTicks must resend, not wait one more tick.
	CHECK(DecideCustody(130, 100, 1, config, false) == CustodyAction::Resend);
}

TEST(CustodyResendsWhileAttemptsRemain) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	CHECK(DecideCustody(500, 100, 2, config, false) == CustodyAction::Resend);
}

TEST(CustodyHoldsRatherThanReclaimingWhenAttemptsAreExhaustedButSendsSucceeded) {
	// THE REGRESSION TEST FOR THE DUPLICATION DEFECT. attempts == maxAttempts means
	// all three sends have happened and none was acked - but every one of them was
	// DELIVERED, so the peer is alive and the object is either in flight or already
	// installed there. Reclaiming here is what produced conservation_delta of +627 and
	// +13,532: the sender applied its own transfer back to itself while the receiver
	// was still applying the original, and both then owned the object.
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	CHECK(DecideCustody(500, 100, 3, config, false) == CustodyAction::Hold);
	CHECK(DecideCustody(500, 100, 4, config, false) == CustodyAction::Hold);
	// However long it has been outstanding. Time alone is never evidence.
	CHECK(DecideCustody(100000000, 100, 99, config, false) == CustodyAction::Hold);
}

TEST(CustodyHoldsForeverWhenOnlyOneAttemptIsAllowedAndItWasDelivered) {
	// maxAttempts 1 used to mean "reclaim on the first timeout" - the most aggressive
	// possible time-gated reclaim. It now means "stop counting attempts", not "reclaim".
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 1;
	CHECK(DecideCustody(130, 100, 1, config, false) == CustodyAction::Hold);
}

TEST(CustodyReclaimsOnlyWhenThePeerLinkIsGone) {
	// The one admissible piece of evidence: there is no peer link to that server at
	// all, so the resend could not even be attempted. The object exists nowhere and no
	// amount of further resending can change that, so the sender takes it back.
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	CHECK(DecideCustody(500, 100, 3, config, true) == CustodyAction::Reclaim);
}

TEST(CustodyReclaimsOnAMissingPeerLinkWithoutWaitingForTheDeadline) {
	// A missing peer link is evidence, not a timeout, so there is nothing to wait for.
	// Deferring to the next deadline would leave the object existing nowhere for
	// another full retry window.
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	// Well inside the retry window, and attempts nowhere near exhausted.
	CHECK(DecideCustody(101, 100, 1, config, true) == CustodyAction::Reclaim);
	// Even with the clock behind the send tick, which otherwise forces Wait.
	CHECK(DecideCustody(50, 100, 1, config, true) == CustodyAction::Reclaim);
}

TEST(CustodyDisabledByZeroRetryTicksNeverActs) {
	// retryTicks 0 restores exactly the pre-custody behaviour, so the two can be
	// compared in one experiment. It must never resend AND never reclaim, however
	// long the transfer has been outstanding.
	CustodyConfig config;
	config.retryTicks = 0;
	config.maxAttempts = 3;
	CHECK(DecideCustody(100000, 100, 1, config, false) == CustodyAction::Wait);
	CHECK(DecideCustody(100000, 100, 99, config, false) == CustodyAction::Wait);
	// Including the undeliverable case: with retry disabled no resend is ever issued,
	// so there is no delivery result to act on and the escape hatch must stay inert.
	CHECK(DecideCustody(100000, 100, 99, config, true) == CustodyAction::Wait);
}

TEST(CustodyWaitsIfTheTickCounterIsBehindTheSendTick) {
	// Defensive: a repartition or a reset must not produce a huge unsigned elapsed
	// value and trigger an instant action.
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	CHECK(DecideCustody(50, 100, 1, config, false) == CustodyAction::Wait);
}

TEST(CustodyNeverReclaimsOnElapsedTimeAloneAtAnyAttemptCount) {
	// Swept assertion of the controller ruling: across the whole (attempts, elapsed)
	// space, with the peer link intact, NO input may produce Reclaim.
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	for (int attempts = 1; attempts <= 50; ++attempts) {
		for (uint64_t now = 100; now <= 100000; now += 997) {
			CHECK(DecideCustody(now, 100, attempts, config, false) != CustodyAction::Reclaim);
		}
	}
}

TEST(CustodyStillBoundsResendsByMaxAttempts) {
	// Hold must not become "resend forever". A resend is only safe while the receiver
	// still HOLDS the object - it acks the duplicate and custody discharges. Once the
	// receiver has handed the object onward (tens of ticks on a moving workload, far
	// less than the retry deadline), a late resend re-installs an object that now lives
	// elsewhere: duplication by a second route. An unbounded-resend build was measured
	// doing 4,117 resends against 199 transfers for conservation_delta +1,463.
	// maxAttempts is what bounds that, so past it the answer must be Hold, never
	// Resend, however long the transfer has been outstanding.
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	for (int attempts = 3; attempts <= 200; ++attempts) {
		for (uint64_t now = 130; now <= 100000; now += 997) {
			CHECK(DecideCustody(now, 100, attempts, config, false) == CustodyAction::Hold);
		}
	}
}

// --- CustodyTicksToMicros -------------------------------------------------------
//
// Task 6a Finding 4: the ticks->microseconds conversion used to live inline in
// ServerWorldManager::SetCustodyConfig, untested. Pulled out to sit next to
// DecideCustody so the same "pure, unit-tested" rule applies to it.

TEST(TicksToMicrosConvertsAtTheNominalRate) {
	// 30 ticks at 120 Hz (dt = 1/120s) is 250ms = 250,000us.
	CHECK_EQ(CustodyTicksToMicros(30, 1.0 / 120.0, 2000000000), 250000);
}

TEST(TicksToMicrosUsesTheGivenRateNotAHardcoded120Hz) {
	// 30 ticks at 60 Hz (dt = 1/60s) is 500ms.
	CHECK_EQ(CustodyTicksToMicros(30, 1.0 / 60.0, 2000000000), 500000);
}

TEST(TicksToMicrosFallsBackTo120HzForANonPositiveRate) {
	CHECK_EQ(CustodyTicksToMicros(30, 0.0, 2000000000), 250000);
	CHECK_EQ(CustodyTicksToMicros(30, -1.0, 2000000000), 250000);
}

TEST(TicksToMicrosClampsNegativeTicksToZero) {
	CHECK_EQ(CustodyTicksToMicros(-5, 1.0 / 120.0, 2000000000), 0);
}

TEST(TicksToMicrosIsCappedRatherThanOverflowing) {
	// A very large configured tick count must not silently overflow; it saturates
	// at the caller-supplied ceiling instead.
	CHECK_EQ(CustodyTicksToMicros(1000000000, 1.0 / 120.0, 2000000000), 2000000000);
}

// --- ScaleCustodyRetryMicros -----------------------------------------------------
//
// Task 6a Finding 4: the batch-size scaling and cap used to live inline in
// ServerWorldManager::FlushPendingTransfers, untested - which is precisely the
// boundary Finding 1 caught (a cap alone can clamp BELOW the configured budget).

TEST(ScaledRetryForALoneTransferIsTheColdConnectionFloorWhenItExceedsConfigured) {
	// batchSize 1: no batch scaling applies. A small configured budget (10ms) is
	// still raised to the cold-connection floor (1s here), because a lone crossing
	// can be the first reliable packet on a still-cold peer connection.
	CHECK_EQ(ScaleCustodyRetryMicros(10000, 1, 10000000, 1000000), 1000000);
}

TEST(ScaledRetryForALoneTransferKeepsConfiguredWhenAboveTheFloor) {
	// batchSize 1, configured (2s) already above the cold-connection floor (1s) and
	// the cap (10s doesn't apply since scaled == configured here): unchanged.
	CHECK_EQ(ScaleCustodyRetryMicros(2000000, 1, 10000000, 1000000), 2000000);
}

TEST(ScaledRetryGrowsLinearlyWithBatchSizeUnderTheCap) {
	// 250ms budget, 4 simultaneous handoffs, well under the 10s cap: exactly 4x.
	CHECK_EQ(ScaleCustodyRetryMicros(250000, 4, 10000000, 1000000), 1000000);
}

TEST(ScaledRetryIsBoundedByTheCapForALargeBatch) {
	// 250ms budget x 64 (MAX_HANDOFFS_PER_TICK) would be 16s; capped at 10s.
	CHECK_EQ(ScaleCustodyRetryMicros(250000, 64, 10000000, 1000000), 10000000);
}

TEST(ScaledRetryNeverDropsBelowTheConfiguredBudgetEvenWhenTheCapWouldClampBelowIt) {
	// Finding 1's exact boundary: a configured budget (15s) that already exceeds the
	// cap (10s) must survive unshortened, even at batchSize 1 (scaled == configured,
	// so min(scaled, cap) alone would wrongly clamp it down to the cap).
	CHECK_EQ(ScaleCustodyRetryMicros(15000000, 1, 10000000, 1000000), 15000000);
}

TEST(ScaledRetryNeverDropsBelowConfiguredEvenWithABatchThatWouldOtherwiseClampToTheCap) {
	// Same boundary, but with a batch size that pushes the naive scaled value even
	// further past the cap - the configured floor must still win.
	CHECK_EQ(ScaleCustodyRetryMicros(15000000, 8, 10000000, 1000000), 15000000);
}

TEST(ScaledRetryTreatsANonPositiveBatchSizeAsOne) {
	CHECK_EQ(ScaleCustodyRetryMicros(250000, 0, 10000000, 1000000), 1000000);
	CHECK_EQ(ScaleCustodyRetryMicros(250000, -3, 10000000, 1000000), 1000000);
}

TEST(ScaledRetryTreatsANegativeConfiguredBudgetAsDisabledLikeZero) {
	// Same reasoning as ScaledRetryOfZeroStaysZeroDespiteTheFloorAndBatchSize below:
	// <= 0 is DecideCustody's own "disabled" boundary, so a negative input must not
	// be resurrected into the cold-connection floor either.
	CHECK_EQ(ScaleCustodyRetryMicros(-5, 4, 10000000, 1000000), 0);
}

TEST(ScaledRetryOfZeroStaysZeroDespiteTheFloorAndBatchSize) {
	// Regression test: CustodyConfig::retryTicks documents 0 as "disables retry
	// entirely - restores the original (pre-custody) behaviour" (HandoffCustody.h),
	// which --handoff-retry-ticks 0 relies on to let a measurement run compare
	// custody-on against custody-off on one binary. The floor and cap both exist to
	// stop a PREMATURE timeout; disabled is not a timeout at all, so neither may
	// resurrect a configured 0 into a non-zero deadline - that would stop
	// DecideCustody's `config.retryTicks <= 0` check from ever seeing zero and
	// silently re-enable custody an operator explicitly turned off. Must hold
	// regardless of batch size, cap, or floor.
	CHECK_EQ(ScaleCustodyRetryMicros(0, 1, 10000000, 1000000), 0);
	CHECK_EQ(ScaleCustodyRetryMicros(0, 64, 10000000, 1000000), 0);
	CHECK_EQ(ScaleCustodyRetryMicros(0, 1, 0, 1000000), 0);
}

// --- IsDuplicateHandoffArrival -------------------------------------------------
//
// The receiver-side half of custody. A resend is a second copy of a transfer the
// receiver may already have installed; ServerWorldManager::ApplyIncomingObject was
// not idempotent below its pool lookup (it re-pushed the object into mTestObjects,
// which is both the population count behind analyse.py's conservation_delta and the
// list Update(dt) iterates), so every redundant arrival added a phantom object AND a
// second application of that object's control forces per tick.
//
// ApplyIncomingObject itself needs a live GameWorld, PhysicsSystem and object pool,
// so it is not unit-testable in this harness. The DECISION it makes is, and that is
// what is factored out here.

TEST(DuplicateGuardFiresForAnObjectAlreadyOwnedAndSimulatingHere) {
	CHECK(IsDuplicateHandoffArrival(false, true, true, false));
}

TEST(DuplicateGuardDoesNotFireForAFirstArrivalTheServerHasNeverSeen) {
	// Pool miss: the normal construct-on-arrival case. Must run the full path.
	CHECK(!IsDuplicateHandoffArrival(false, false, false, false));
}

TEST(DuplicateGuardDoesNotFireForAHaloShadow) {
	// A shadow is present in the pool but is a READ-ONLY copy of a neighbour's
	// object. Its handoff is a promotion, not a duplicate - swallowing it would
	// leave the object permanently unowned and never integrated. Asserted with
	// networkActive both true and false, since a shadow's activity is irrelevant
	// to the decision.
	CHECK(!IsDuplicateHandoffArrival(false, true, true, true));
	CHECK(!IsDuplicateHandoffArrival(false, true, false, true));
}

TEST(DuplicateGuardDoesNotFireForAKnownButInactiveEntry) {
	// Present in the pool but not network-active: known id, not installed. Only a
	// completed installation makes the arrival redundant, so this must fall through
	// to the shared registration path that finishes the job.
	CHECK(!IsDuplicateHandoffArrival(false, true, false, false));
}

TEST(DuplicateGuardIsSkippedEntirelyForAReclaim) {
	// A reclaim is the SENDER re-applying its own transfer to itself after the peer
	// link died. IsDuplicateHandoffArrival is not the predicate that guards it: a
	// reclaim of an object that genuinely left is a re-installation, and suppressing
	// it would strand the object nowhere at all, which is the loss custody exists to
	// prevent. So this predicate is exempt for every combination of the other inputs.
	//
	// That exemption is NOT the same as "a reclaim always runs the full apply path".
	// The still-present case is caught by IsRedundantReclaim below instead - see the
	// tests that follow. Keeping the two predicates separate is deliberate: this one
	// answers "is this a redundant DELIVERY", that one answers "is this a redundant
	// RECLAIM", and the correct responses differ (the duplicate is acked and ignored,
	// the redundant reclaim still has ownership bookkeeping to restore).
	CHECK(!IsDuplicateHandoffArrival(true, true, true, false));
	CHECK(!IsDuplicateHandoffArrival(true, true, true, true));
	CHECK(!IsDuplicateHandoffArrival(true, false, false, false));
	CHECK(!IsDuplicateHandoffArrival(true, true, false, false));
}

TEST(DuplicateGuardTreatsANulledPoolEntryAsAbsent) {
	// A destroyed object leaves a permanent tombstone with the pool entry NULLED
	// rather than erased (CLAUDE.md, runtime spawn/destroy). ApplyIncomingObject maps
	// that to objectPresent = false - it reads `entry->second != nullptr`, so a nulled
	// entry is absent and, being absent, cannot be network-active either. That is the
	// half that matters, and the combination the call site can actually produce:
	// objectPresent = false with networkActive = false.
	CHECK(!IsDuplicateHandoffArrival(false, false, false, false));
	// The isHaloShadow flag is derived from the same null check, so it is false too.
	CHECK(!IsDuplicateHandoffArrival(false, false, false, true));
	// Defensive: even if a caller ever passed the impossible (absent yet active)
	// combination, absence must still win, so the destroy handling above the guard
	// keeps its precedence rather than the arrival being swallowed as a duplicate.
	CHECK(!IsDuplicateHandoffArrival(false, false, true, false));
}

// --- IsRedundantReclaim ---------------------------------------------------------
//
// THE REGRESSION TESTS FOR THE RECLAIM-BEFORE-RELEASE DEFECT.
//
// The release of a handed-off object is scheduled on a SIMULATION tick
// (ScheduleOutgoingObject: senderTick + --handoff-lookahead, i.e. 2.5s of simulated
// time at the lookahead 300 that E2 and E4 both use). The reclaim deadline is
// WALL-CLOCK, and a missing peer link bypasses it entirely, so the earliest possible
// reclaim is the 1s cold-connection floor. A peer that dies right after a transfer is
// sent therefore gets reclaimed BEFORE the object it carried has been released.
//
// Before this predicate existed, that ran ApplyIncomingObject on a still-present,
// still-active object with isReclaim = true, which bypasses IsDuplicateHandoffArrival
// and re-pushed the object into mTestObjects; FlushScheduledReleases then still fired
// HandleOutgoingObject at releaseAtTick and shipped it to a peer already proven gone.
// hoSent and hoRecv had both been incremented, so ho_parity_delta and hoCustody both
// read 0 and the loss was invisible.

TEST(RedundantReclaimFiresForAnObjectStillPresentAndActiveHere) {
	// The exact defect: reclaimed before its own scheduled release ran, so the object
	// never left. Nothing to re-install.
	CHECK(IsRedundantReclaim(true, true, true, false));
}

TEST(RedundantReclaimDoesNotFireForAnObjectThatGenuinelyLeft) {
	// The normal reclaim. HandleOutgoingObject ERASED the pool entry, so the object is
	// absent and must be constructed on arrival - the whole purpose of a reclaim.
	CHECK(!IsRedundantReclaim(true, false, false, false));
}

TEST(RedundantReclaimDoesNotFireForAHaloShadow) {
	// Present in the pool, but as a neighbour's read-only copy rather than our
	// retained object. Its reclaim must run the full promotion path, exactly as a
	// normal handoff of a shadow does.
	CHECK(!IsRedundantReclaim(true, true, true, true));
	CHECK(!IsRedundantReclaim(true, true, false, true));
}

TEST(RedundantReclaimDoesNotFireForAKnownButInactiveEntry) {
	// Present but not network-active: a husk mid-installation, not a completed one.
	// The shared registration path still has work to do, so this must fall through.
	CHECK(!IsRedundantReclaim(true, true, false, false));
}

TEST(RedundantReclaimNeverFiresForAnOrdinaryArrival) {
	// It is a reclaim-only rule. An ordinary (non-reclaim) arrival is judged by
	// IsDuplicateHandoffArrival, which counts hoDup and does NOT touch ownership
	// bookkeeping; routing one through the reclaim short-circuit instead would lose
	// that counter. Swept across every other input.
	for (int present = 0; present <= 1; ++present) {
		for (int active = 0; active <= 1; ++active) {
			for (int shadow = 0; shadow <= 1; ++shadow) {
				CHECK(!IsRedundantReclaim(false, present != 0, active != 0, shadow != 0));
			}
		}
	}
}

TEST(RedundantReclaimAndDuplicateGuardAreNeverBothTrue) {
	// The two predicates partition the short-circuit space rather than overlapping:
	// IsDuplicateHandoffArrival is exempt for isReclaim, IsRedundantReclaim requires
	// it. ApplyIncomingObject tests them in sequence, so an input satisfying both
	// would make the order of those two branches load-bearing and silent.
	for (int reclaim = 0; reclaim <= 1; ++reclaim) {
		for (int present = 0; present <= 1; ++present) {
			for (int active = 0; active <= 1; ++active) {
				for (int shadow = 0; shadow <= 1; ++shadow) {
					const bool dup = IsDuplicateHandoffArrival(
						reclaim != 0, present != 0, active != 0, shadow != 0);
					const bool redundant = IsRedundantReclaim(
						reclaim != 0, present != 0, active != 0, shadow != 0);
					CHECK(!(dup && redundant));
				}
			}
		}
	}
}
