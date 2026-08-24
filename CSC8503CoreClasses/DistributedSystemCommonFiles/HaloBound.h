#pragma once
#include <algorithm>

// The halo band's soundness condition, as pure arithmetic.
//
// Extracted from ServerWorldManager so it can be asserted directly in
// tools/InteractionTests: it is the headline correctness claim of the cross-border
// collision increment, it is four multiplications, and a claim that cheap to check
// should not only be checkable by running the whole distributed system.
//
// Deliberately free of USEGL / DISTRIBUTEDSYSTEMACTIVE guards, like
// InteractionCommand.h and RegionOwnership.h, because the servers and the test target
// all include it.
namespace NCL::Distributed {

	// Conservative by construction: an ASSUMED maximum speed, not the speed a workload
	// actually runs at, so the width this yields is an upper bound on what is needed
	// rather than an estimate of it. An object faster than this falls outside the
	// guarantee - which is a statement about the constant, not about the bound.
	constexpr float HALO_ASSUMED_MAX_SPEED = 60.0f;
	// Generous compared with the unit cubes the workloads build, so a pair is
	// published well before it can touch.
	constexpr float HALO_ASSUMED_MAX_RADIUS = 2.0f;

	constexpr int HALO_DEFAULT_SUBSTEP_HZ = 120;

	// w_min = v_max * (L * dt + T_L + T_J) + 2 * r_max
	//
	// The bracket is the total lag between an object's state being SAMPLED by its
	// owner and APPLIED on the neighbour:
	//
	//   L * dt   deliberate scheduling lookahead, in substeps
	//   T_L      one-way link latency
	//   T_J      worst-case jitter on top of it
	//
	// Jitter enters at its maximum rather than its mean because this is a bound: a
	// band sized for average delay misses contacts on the slow tail, and a bound that
	// holds on average is not a bound.
	//
	// At T_L = T_J = 0 this returns exactly `v_max * L * dt + 2 * r_max`, the
	// published zero-latency expression - which is what keeps the existing E5 sweep
	// valid as a baseline. That identity is asserted in HaloBoundTests.
	inline float MinimumSafeHaloWidth(int lookaheadTicks,
		int substepHz = HALO_DEFAULT_SUBSTEP_HZ,
		float linkLatencyMs = 0.0f,
		float linkJitterMs = 0.0f) {
		const float substepDt = (substepHz > 0)
			? (1.0f / static_cast<float>(substepHz))
			: (1.0f / static_cast<float>(HALO_DEFAULT_SUBSTEP_HZ));

		const float schedulingLag = static_cast<float>(std::max(0, lookaheadTicks)) * substepDt;
		// Clamped rather than trusted: a negative injected delay is a configuration
		// error, and letting it SHRINK the safe width would turn a misconfiguration
		// into silently missed contacts.
		const float linkLag = std::max(0.0f, linkLatencyMs) * 0.001f
			+ std::max(0.0f, linkJitterMs) * 0.001f;

		return HALO_ASSUMED_MAX_SPEED * (schedulingLag + linkLag)
			+ 2.0f * HALO_ASSUMED_MAX_RADIUS;
	}

	// Ticks a halo shadow may go without an applied update before it is retired.
	//
	// Must clear the scheduling lookahead as well as the drop tolerance. With a
	// lookahead of L a shadow legitimately goes L ticks between applications even when
	// every packet arrives, so a FIXED horizon becomes a ceiling on lookahead: above
	// it, a shadow is retired before the update that would refresh it is due, the halo
	// silently stops working, and the run reports a staleness horizon as though it
	// were a missed contact. That is how L=32 came to read as unsound in E5 round 1.
	constexpr int HALO_STALE_DROP_TOLERANCE = 30;

	inline int HaloStaleTicks(int lookaheadTicks) {
		return HALO_STALE_DROP_TOLERANCE + std::max(0, lookaheadTicks);
	}
}
