#include "TestHarness.h"

#include "DistributedSystemCommonFiles/HaloBound.h"

#include <cmath>

using namespace NCL::Distributed;

namespace {
	// The published zero-latency expression, written out longhand rather than
	// refactored. The whole point of gate 4.4.2 is that the generalised form still
	// produces THIS, so calling the generalised function to compute the expected value
	// would assert nothing at all.
	float PublishedZeroLatencyFloor(int lookaheadTicks, int substepHz) {
		const float dt = 1.0f / static_cast<float>(substepHz);
		return 60.0f * (static_cast<float>(lookaheadTicks) * dt) + 2.0f * 2.0f;
	}

	bool NearlyEqual(float a, float b) {
		return std::fabs(a - b) < 1e-4f;
	}
}

// Phase C gate 4.4.2: the generalised bound must be a strict superset of the one E5
// validated. If this fails, the existing 120-run zero-latency sweep stops being a
// valid baseline and Phase C loses its reference point.
TEST(GeneralisedBoundReducesToThePublishedOneAtZeroLatency) {
	for (int lookahead : {0, 2, 4, 8, 16, 24, 30}) {
		const float generalised = MinimumSafeHaloWidth(lookahead, 120, 0.0f, 0.0f);
		const float published = PublishedZeroLatencyFloor(lookahead, 120);
		CHECK(NearlyEqual(generalised, published));
	}
}

// The published figures E5 quotes, so a change to the constants shows up here rather
// than silently moving the floor every result in that section is compared against.
TEST(BoundMatchesThePublishedFloorsAtTheSweptLookaheads) {
	// w_min = 60 * L/120 + 4  =  L/2 + 4
	CHECK(NearlyEqual(MinimumSafeHaloWidth(2, 120), 5.0f));
	CHECK(NearlyEqual(MinimumSafeHaloWidth(8, 120), 8.0f));
	CHECK(NearlyEqual(MinimumSafeHaloWidth(16, 120), 12.0f));
	CHECK(NearlyEqual(MinimumSafeHaloWidth(24, 120), 16.0f));
}

// Latency enters as travel distance at the assumed maximum speed: 60 units/s over
// T_L seconds. At 50 ms that is exactly 3 units on top of the zero-latency floor.
TEST(LatencyAddsTravelDistanceAtTheAssumedMaximumSpeed) {
	const float base = MinimumSafeHaloWidth(4, 120, 0.0f, 0.0f);
	CHECK(NearlyEqual(MinimumSafeHaloWidth(4, 120, 50.0f, 0.0f), base + 3.0f));
	CHECK(NearlyEqual(MinimumSafeHaloWidth(4, 120, 100.0f, 0.0f), base + 6.0f));
}

// Jitter is worst-case, so it adds on the same footing as latency rather than being
// averaged in. A bound that held only on the mean would not be a bound.
TEST(JitterAddsAtItsMaximumNotItsMean) {
	CHECK(NearlyEqual(MinimumSafeHaloWidth(4, 120, 50.0f, 0.0f),
		MinimumSafeHaloWidth(4, 120, 0.0f, 50.0f)));
	CHECK(NearlyEqual(MinimumSafeHaloWidth(4, 120, 25.0f, 25.0f),
		MinimumSafeHaloWidth(4, 120, 50.0f, 0.0f)));
}

// A negative injected delay is a misconfiguration. It must never SHRINK the safe
// width, or a typo turns into silently missed contacts rather than an error.
TEST(NegativeDelaysCannotShrinkTheBound) {
	const float base = MinimumSafeHaloWidth(4, 120, 0.0f, 0.0f);
	CHECK(NearlyEqual(MinimumSafeHaloWidth(4, 120, -50.0f, 0.0f), base));
	CHECK(NearlyEqual(MinimumSafeHaloWidth(4, 120, 0.0f, -50.0f), base));
	CHECK(NearlyEqual(MinimumSafeHaloWidth(-4, 120, 0.0f, 0.0f),
		MinimumSafeHaloWidth(0, 120, 0.0f, 0.0f)));
}

// The §4.2 precondition. A fixed staleness horizon is a ceiling on lookahead: above
// it a shadow is retired before the update that would refresh it is due to apply, and
// the halo silently stops working. E5 round 1 read exactly that as unsoundness at
// L=32, which is why the horizon now tracks the lookahead.
TEST(StalenessHorizonClearsTheSchedulingLookahead) {
	for (int lookahead : {0, 4, 24, 32, 64}) {
		CHECK(HaloStaleTicks(lookahead) > lookahead);
	}
	// The old fixed value is the drop tolerance, and it is preserved at zero
	// lookahead - the horizon grows from it rather than replacing it.
	CHECK_EQ(HaloStaleTicks(0), 30);
	CHECK_EQ(HaloStaleTicks(32), 62);
	// The case that broke: a fixed 30 would retire before an L=32 update was due.
	CHECK(HaloStaleTicks(32) > 30);
}
