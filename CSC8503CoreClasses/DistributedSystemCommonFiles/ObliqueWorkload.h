#pragma once
#include <cmath>

#include "HaloBound.h"

// The "oblique" workload's launch geometry.
//
// `headon` is one synthetic workload: fixed lanes, ONE speed, and a strictly
// perpendicular approach, so every pair meets at exactly x = 0 at exactly the same
// instant. That is the configuration where the halo's knee is sharpest, and it is the
// only workload behind the ~200 ms total-lag ceiling recorded in
// docs/superpowers/results/2026-08-19-E5-soundness.md. Whether that ceiling is a
// property of the DESIGN or of how collision-dense headon is at the border cannot be
// told from headon alone - see the Phase C spec S4.7 and roadmap S6.2 item 3.
//
// This varies two things headon holds fixed, while keeping everything else identical:
//
//  - the APPROACH ANGLE. A pair still converges on x = 0, but travels obliquely, so
//    dead-reckoning error acquires a TANGENTIAL component. headon never exercises that:
//    with a purely perpendicular approach, a shadow's placement error is collinear with
//    the closing direction, which is the easiest case for a band measured perpendicular
//    to the border.
//  - the SPEED. Objects no longer all move at one speed, so `HALO_ASSUMED_MAX_SPEED` is
//    a genuine upper bound rather than the exact speed of every object - which is the
//    premise the published bound is stated under and has never been tested against.
//
// The PERPENDICULAR component is deliberately held at headon's own speed, so
// time-to-contact is identical for every lane and identical to headon. That matters:
// varying it would change how long a pair falls before meeting, and headon's constants
// are already pinned between "too slow and it lands first" and "too fast and it
// tunnels". Holding it fixed keeps this workload inside both limits by construction and
// keeps the comparison against headon honest - only the angle and the magnitude differ.
//
// Consequently speed is a DERIVED quantity: |v| = perpSpeed / cos(theta).

namespace NCL::Distributed {

	// The launch velocity for one member of an oblique pair. Y is always 0 - the
	// workload is planar, exactly as headon is.
	struct ObliqueLaunch {
		float vx = 0.0f;
		float vz = 0.0f;
	};

	// The approach angle for a lane, in radians.
	//
	// Linear in the lane index, and that is load-bearing rather than convenient:
	// ADJACENT LANES MUST HAVE NEARLY EQUAL ANGLES. Their tangential speeds then differ
	// by a hair, so neighbouring pairs barely drift relative to one another and stay
	// inside their lane for the length of a run. Assigning angles randomly per lane
	// would let two adjacent lanes draw opposite extremes, drift into each other, and
	// contaminate the contact count with collisions that are not the head-on one - which
	// is the whole reason headon's lanes are spaced the way they are.
	//
	// A single-lane run gets angle 0, i.e. exactly headon.
	//
	// The SERVER does not call this one. It calls ObliqueAngleAtFraction below, keyed on
	// the object's own z position, because `headon` already establishes that deriving a
	// workload property from the grid index means keeping the index-to-cell mapping in
	// step in a second place. Position cannot drift from itself. This lane-indexed form
	// is the same function expressed over lanes, and exists so the property tests can
	// talk about lanes directly.
	inline float ObliqueAngleAtFraction(float fraction, float maxAngleRadians) {
		if (fraction < 0.0f) {
			fraction = 0.0f;
		}
		if (fraction > 1.0f) {
			fraction = 1.0f;
		}
		return fraction * maxAngleRadians;
	}

	inline float ObliqueLaneAngle(int laneIndex, int laneCount, float maxAngleRadians) {
		if (laneCount <= 1) {
			return 0.0f;
		}
		if (laneIndex < 0) {
			laneIndex = 0;
		}
		if (laneIndex >= laneCount) {
			laneIndex = laneCount - 1;
		}
		const float fraction = static_cast<float>(laneIndex)
			/ static_cast<float>(laneCount - 1);
		return ObliqueAngleAtFraction(fraction, maxAngleRadians);
	}

	// Velocity for one member of the pair in `laneIndex`.
	//
	// `leftSide` is the member at negative x, which travels in +x; its partner travels
	// in -x. Both carry the SAME tangential (z) component, which is what keeps them in
	// one plane so they still meet: they converge in x while drifting together in z.
	// Mirroring the z component instead would make them miss.
	inline ObliqueLaunch ObliqueVelocityAtAngle(float theta, float perpSpeed, bool leftSide) {
		ObliqueLaunch launch;
		launch.vx = leftSide ? perpSpeed : -perpSpeed;
		launch.vz = perpSpeed * std::tan(theta);
		return launch;
	}

	inline ObliqueLaunch ObliqueLaneVelocity(int laneIndex, int laneCount,
		float perpSpeed, float maxAngleRadians, bool leftSide) {
		return ObliqueVelocityAtAngle(
			ObliqueLaneAngle(laneIndex, laneCount, maxAngleRadians), perpSpeed, leftSide);
	}

	// |v| for a lane. Derived, not configured: the perpendicular component is fixed, so
	// the magnitude is perpSpeed / cos(theta) and rises with the angle.
	inline float ObliqueLaneSpeed(int laneIndex, int laneCount,
		float perpSpeed, float maxAngleRadians) {
		const float theta = ObliqueLaneAngle(laneIndex, laneCount, maxAngleRadians);
		return perpSpeed / std::cos(theta);
	}

	// The largest |v| this workload will launch, i.e. the value at the widest angle.
	//
	// The soundness bound is stated under `HALO_ASSUMED_MAX_SPEED`; a workload that
	// exceeded it would be testing the bound against a violated premise and any failure
	// would say nothing about the bound. The server asserts this at construction rather
	// than trusting the constants to stay compatible.
	inline float ObliqueMaxSpeed(float perpSpeed, float maxAngleRadians) {
		return perpSpeed / std::cos(maxAngleRadians);
	}

	inline bool ObliqueSpeedIsWithinBound(float perpSpeed, float maxAngleRadians) {
		return ObliqueMaxSpeed(perpSpeed, maxAngleRadians) <= HALO_ASSUMED_MAX_SPEED;
	}
}
