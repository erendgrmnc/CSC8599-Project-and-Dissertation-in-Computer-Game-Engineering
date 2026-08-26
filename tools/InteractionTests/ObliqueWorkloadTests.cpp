// The oblique workload's launch geometry.
//
// This exists because the ~200 ms total-lag ceiling on the halo soundness condition was
// measured on ONE workload - headon - which is fixed-lane, single-speed and strictly
// perpendicular. Whether the ceiling is a property of the design or an artefact of how
// collision-dense headon is at the border cannot be told from headon alone. These tests
// pin the two properties that make the new workload a fair second opinion rather than a
// different experiment: pairs must still meet, and speeds must stay inside the bound the
// soundness condition is stated under.

#include "TestHarness.h"

#include "DistributedSystemCommonFiles/ObliqueWorkload.h"

using namespace NCL::Distributed;

namespace {
	constexpr float OBLIQUE_TEST_PERP = 30.0f;                 // headon's own speed
	constexpr float OBLIQUE_TEST_MAX_ANGLE = 0.7853981634f;    // 45 degrees
}

TEST(ObliqueFirstLaneIsExactlyHeadon) {
	// Lane 0 has angle 0, so the workload degenerates to headon there. That is what
	// makes the two comparable: the same run contains the old workload as its first
	// lane and departs from it continuously.
	const ObliqueLaunch left = ObliqueLaneVelocity(0, 8, OBLIQUE_TEST_PERP,
		OBLIQUE_TEST_MAX_ANGLE, true);
	CHECK_EQ(left.vx, OBLIQUE_TEST_PERP);
	CHECK_EQ(left.vz, 0.0f);
}

TEST(ObliqueSingleLaneIsExactlyHeadon) {
	// laneCount 1 would divide by zero on the naive fraction. It must degenerate, not
	// crash or produce a nan that silently propagates into a velocity.
	const ObliqueLaunch left = ObliqueLaneVelocity(0, 1, OBLIQUE_TEST_PERP,
		OBLIQUE_TEST_MAX_ANGLE, true);
	CHECK_EQ(left.vz, 0.0f);
	CHECK_EQ(left.vx, OBLIQUE_TEST_PERP);
}

TEST(ObliquePairConvergesInXAndTravelsTogetherInZ) {
	// THE PROPERTY THE WORKLOAD DEPENDS ON. The pair must still collide, or the run
	// measures nothing. They converge in x (opposite vx) while sharing one z velocity,
	// so they stay in a single plane and meet. Mirroring z instead would make them miss,
	// and the contact count would silently drop to zero with no error anywhere.
	for (int lane = 0; lane < 6; ++lane) {
		const ObliqueLaunch left = ObliqueLaneVelocity(lane, 6, OBLIQUE_TEST_PERP,
			OBLIQUE_TEST_MAX_ANGLE, true);
		const ObliqueLaunch right = ObliqueLaneVelocity(lane, 6, OBLIQUE_TEST_PERP,
			OBLIQUE_TEST_MAX_ANGLE, false);
		CHECK_EQ(left.vx, -right.vx);   // converging
		CHECK_EQ(left.vz, right.vz);    // same plane
	}
}

TEST(ObliquePerpendicularComponentIsIdenticalOnEveryLane) {
	// Time-to-contact is therefore the same on every lane and the same as headon's. If
	// this varied, lanes would meet at different moments and a slow lane could land
	// before reaching the border - headon's constants are pinned between exactly that
	// and tunnelling.
	for (int lane = 0; lane < 10; ++lane) {
		const ObliqueLaunch left = ObliqueLaneVelocity(lane, 10, OBLIQUE_TEST_PERP,
			OBLIQUE_TEST_MAX_ANGLE, true);
		CHECK_EQ(left.vx, OBLIQUE_TEST_PERP);
	}
}

TEST(ObliqueSpeedRisesWithTheLaneIndex) {
	// The mixed-speed half of the workload. |v| = perp / cos(theta), so it is strictly
	// increasing across lanes rather than randomly assorted.
	float previous = 0.0f;
	for (int lane = 0; lane < 8; ++lane) {
		const float speed = ObliqueLaneSpeed(lane, 8, OBLIQUE_TEST_PERP,
			OBLIQUE_TEST_MAX_ANGLE);
		CHECK(speed > previous);
		previous = speed;
	}
}

TEST(ObliqueAdjacentLanesHaveNearlyEqualAngles) {
	// Why the angle is linear in the lane index rather than drawn from the seed.
	// Neighbouring pairs must not drift into one another, or contacts that are not the
	// head-on one enter the count and the acceptance test stops measuring what it claims.
	const int lanes = 50;
	for (int lane = 1; lane < lanes; ++lane) {
		const float a = ObliqueLaneAngle(lane - 1, lanes, OBLIQUE_TEST_MAX_ANGLE);
		const float b = ObliqueLaneAngle(lane, lanes, OBLIQUE_TEST_MAX_ANGLE);
		CHECK((b - a) < 0.02f);   // ~1.1 degrees at 50 lanes
	}
}

TEST(ObliqueSpeedStaysInsideTheBoundsAssumedMaximum) {
	// THE PREMISE CHECK. The soundness condition is stated under HALO_ASSUMED_MAX_SPEED.
	// A workload launching faster than that would test the bound against a violated
	// premise, and any missed contact would say nothing about the bound itself.
	CHECK(ObliqueSpeedIsWithinBound(OBLIQUE_TEST_PERP, OBLIQUE_TEST_MAX_ANGLE));
	CHECK(ObliqueMaxSpeed(OBLIQUE_TEST_PERP, OBLIQUE_TEST_MAX_ANGLE)
		<= HALO_ASSUMED_MAX_SPEED);
}

TEST(ObliqueRejectsAConfigurationThatWouldBreachTheBound) {
	// Adversarial: the guard must actually be capable of saying no. At 50 u/s
	// perpendicular and 45 degrees the magnitude is ~70.7, above the assumed 60.
	CHECK(!ObliqueSpeedIsWithinBound(50.0f, OBLIQUE_TEST_MAX_ANGLE));
}

TEST(ObliqueLaneIndexIsClampedRatherThanReadingOutOfRange) {
	const float low = ObliqueLaneAngle(-5, 8, OBLIQUE_TEST_MAX_ANGLE);
	const float high = ObliqueLaneAngle(99, 8, OBLIQUE_TEST_MAX_ANGLE);
	CHECK_EQ(low, 0.0f);
	CHECK_EQ(high, OBLIQUE_TEST_MAX_ANGLE);
}

// --- the position-keyed form the server actually calls --------------------------

TEST(ObliqueFractionFormAgreesWithTheLaneForm) {
	// The two must not drift apart: the lane form is the fraction form expressed over
	// lanes, and the tests above constrain the lane form.
	const int lanes = 9;
	for (int lane = 0; lane < lanes; ++lane) {
		const float viaLane = ObliqueLaneAngle(lane, lanes, OBLIQUE_TEST_MAX_ANGLE);
		const float fraction = static_cast<float>(lane) / static_cast<float>(lanes - 1);
		const float viaFraction = ObliqueAngleAtFraction(fraction, OBLIQUE_TEST_MAX_ANGLE);
        CHECK_NEAR(viaLane, viaFraction, 1e-6f);
	}
}

TEST(ObliqueFractionIsClampedAtBothEnds) {
	// The server derives the fraction from a world position. An object that has drifted
	// outside the world extent must not produce an angle beyond the configured maximum,
	// which would launch it faster than the bound's assumed maximum speed.
	CHECK_EQ(ObliqueAngleAtFraction(-0.5f, OBLIQUE_TEST_MAX_ANGLE), 0.0f);
	CHECK_EQ(ObliqueAngleAtFraction(1.5f, OBLIQUE_TEST_MAX_ANGLE), OBLIQUE_TEST_MAX_ANGLE);
}

TEST(ObliqueVelocityAtAngleZeroIsHeadon) {
	const ObliqueLaunch left = ObliqueVelocityAtAngle(0.0f, OBLIQUE_TEST_PERP, true);
	const ObliqueLaunch right = ObliqueVelocityAtAngle(0.0f, OBLIQUE_TEST_PERP, false);
	CHECK_EQ(left.vx, OBLIQUE_TEST_PERP);
	CHECK_EQ(right.vx, -OBLIQUE_TEST_PERP);
	CHECK_EQ(left.vz, 0.0f);
	CHECK_EQ(right.vz, 0.0f);
}
