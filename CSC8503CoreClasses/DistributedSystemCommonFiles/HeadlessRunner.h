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

		// Ends the run early, cleanly, when it returns true. Checked once per
		// iteration BEFORE the tick, so the caller's own end-of-run reporting runs
		// exactly as it would on a bound being reached.
		//
		// This exists because a bound is the wrong instrument for a role whose
		// natural lifetime is "as long as its peers". The client must outlive the
		// servers - stopping while they are still broadcasting reliable packets at
		// it blocks their loop for tens of seconds - so the harness sized its window
		// past the servers' and then force-killed it. A force-killed process never
		// returns from this loop, so its end-of-run @@FINAL line was never printed:
		// across all 12 Phase A experiments, 60 client logs contained zero of them,
		// which left invariant I4 (command accounting) with nothing to check.
		//
		// A predicate fixes what a bound cannot express: run until the peers are
		// gone, then exit under our own power with the totals intact.
		std::function<bool()> stopWhen;

		// Aligns tick 0 to a shared wall-clock boundary, in microseconds. 0 disables.
		//
		// Every server counts ticks from its own game-start, so a sender's tick number
		// is only meaningful to a receiver if both started on the same tick. The
		// game-start broadcast arrives with a spread of a few milliseconds, which at
		// 120 Hz is enough to shift epochs by a tick and make handoff scheduling differ
		// between runs. Waiting for the next common boundary on the monotonic clock -
		// which QPC makes consistent across processes on one machine - removes that
		// without any inter-server message.
		//
		// Cross-machine this needs real clock synchronisation; on one machine it is
		// exact.
		long long epochAlignMicros = 0;
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
