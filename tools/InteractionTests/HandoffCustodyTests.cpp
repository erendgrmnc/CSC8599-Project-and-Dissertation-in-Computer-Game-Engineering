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
