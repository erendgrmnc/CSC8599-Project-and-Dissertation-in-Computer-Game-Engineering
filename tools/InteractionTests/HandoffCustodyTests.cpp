// The custody decision for an unacknowledged handoff.
//
// Acknowledged transfers are ERASED from mPendingTransfers, so they never reach this
// function - it only ever sees transfers still outstanding. That is what keeps it a
// pure function of (tick, lastSentTick, attempts, config) with no "acked" input.

#include "TestHarness.h"

#include "DistributedSystemCommonFiles/HandoffCustody.h"

using namespace NCL::Distributed;

TEST(CustodyWaitsBeforeTheRetryTimeout) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	// Sent on tick 100, now tick 129: 29 ticks elapsed, one short of the timeout.
	CHECK(DecideCustody(129, 100, 1, config) == CustodyAction::Wait);
}

TEST(CustodyResendsExactlyOnTheTimeoutTick) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	// Boundary: elapsed == retryTicks must resend, not wait one more tick.
	CHECK(DecideCustody(130, 100, 1, config) == CustodyAction::Resend);
}

TEST(CustodyResendsWhileAttemptsRemain) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	CHECK(DecideCustody(500, 100, 2, config) == CustodyAction::Resend);
}

TEST(CustodyReclaimsWhenAttemptsAreExhausted) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	// attempts == maxAttempts means all three sends have happened and none was acked.
	CHECK(DecideCustody(500, 100, 3, config) == CustodyAction::Reclaim);
	CHECK(DecideCustody(500, 100, 4, config) == CustodyAction::Reclaim);
}

TEST(CustodyReclaimsOnFirstTimeoutWhenOnlyOneAttemptIsAllowed) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 1;
	CHECK(DecideCustody(130, 100, 1, config) == CustodyAction::Reclaim);
}

TEST(CustodyDisabledByZeroRetryTicksNeverActs) {
	// retryTicks 0 restores exactly the pre-custody behaviour, so the two can be
	// compared in one experiment. It must never resend AND never reclaim, however
	// long the transfer has been outstanding.
	CustodyConfig config;
	config.retryTicks = 0;
	config.maxAttempts = 3;
	CHECK(DecideCustody(100000, 100, 1, config) == CustodyAction::Wait);
	CHECK(DecideCustody(100000, 100, 99, config) == CustodyAction::Wait);
}

TEST(CustodyWaitsIfTheTickCounterIsBehindTheSendTick) {
	// Defensive: a repartition or a reset must not produce a huge unsigned elapsed
	// value and trigger an instant reclaim.
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	CHECK(DecideCustody(50, 100, 1, config) == CustodyAction::Wait);
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

TEST(ScaledRetryClampsNegativeInputsRatherThanMisbehaving) {
	CHECK_EQ(ScaleCustodyRetryMicros(-5, 4, 10000000, 1000000), 1000000);
}
