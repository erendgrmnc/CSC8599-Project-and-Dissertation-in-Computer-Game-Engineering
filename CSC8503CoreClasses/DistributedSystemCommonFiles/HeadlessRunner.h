#pragma once
#include <functional>

namespace NCL {

	// How a headless run is paced and bounded.
	//
	// There are two modes, and the choice is a methodological one:
	//
	//  * REALTIME (fixedDt <= 0, the default). tick() receives measured wall-clock
	//    deltas, so the run reflects genuine behaviour under real load. It is NOT
	//    reproducible: the number of ticks in a fixed wall-clock window varies with
	//    machine load, and per-tick work such as the server-border check therefore
	//    lands at different simulated times. Two runs of the same binary at the same
	//    seed produce different handoff counts. Use it for performance claims, with
	//    repeats and error bars - never for exact comparison.
	//
	//  * REPRODUCIBLE (fixedDt > 0). tick() receives exactly fixedDt every iteration,
	//    so simulated time is decoupled from wall clock and the run is deterministic:
	//    identical inputs give identical outputs, on any machine, at any speed. The
	//    run no longer takes runSeconds of real time, and it no longer measures
	//    realtime responsiveness. Use it for correctness and conservation experiments.
	//
	// Bounds: runTicks is preferred in reproducible mode because it fixes the amount
	// of SIMULATED time exactly; runSeconds bounds wall-clock time and is what
	// realtime mode wants. If both are set, whichever is reached first ends the run.
	// Both zero runs until the process is terminated externally, the original
	// behaviour.
	struct HeadlessRunOptions {
		double runSeconds = 0.0;
		long long runTicks = 0;
		float fixedDt = 0.0f;

		// Gates the tick budget. Ticks are still executed while this returns false -
		// the role needs them to pump its network and complete the bootstrap - but
		// they do not count toward runTicks, and the loop paces itself normally
		// rather than spinning.
		//
		// Without this, a reproducible run consumes its entire budget during the
		// manager/midware/client handshake and exits before the world is even built:
		// 7200 ticks elapse in ~25ms when nothing is sleeping.
		std::function<bool()> countTicksWhen;
	};

	// Runs a loop calling tick(dt). Used by the distributed roles in --headless mode,
	// where there is no Window / ProfilerRenderer to gate or time the loop.
	//
	// A bounded run returns cleanly rather than being force-killed, which is what
	// gives buffered metrics a chance to be flushed.
	void RunHeadlessLoop(const std::function<void(float dt)>& tick, const HeadlessRunOptions& options);

	// Convenience overload for the realtime, wall-clock-bounded case.
	void RunHeadlessLoop(const std::function<void(float dt)>& tick, double runSeconds = 0.0);
}
