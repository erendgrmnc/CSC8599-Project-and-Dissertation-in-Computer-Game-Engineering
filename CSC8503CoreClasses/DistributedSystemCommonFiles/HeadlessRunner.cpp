#include "HeadlessRunner.h"

#include "GameTimer.h"

#include <thread>
#include <chrono>
#include <iostream>

void NCL::RunHeadlessLoop(const std::function<void(float dt)>& tick, double runSeconds) {
	HeadlessRunOptions options;
	options.runSeconds = runSeconds;
	RunHeadlessLoop(tick, options);
}

void NCL::RunHeadlessLoop(const std::function<void(float dt)>& tick, const HeadlessRunOptions& options) {
	NCL::GameTimer timer;
	timer.Tick();
	timer.GetTimeDeltaSeconds(); // Clear the timer so we don't get a large first dt.

	const auto started = std::chrono::steady_clock::now();
	const bool boundedByTime = (options.runSeconds > 0.0);
	const bool boundedByTicks = (options.runTicks > 0);
	const bool reproducible = (options.fixedDt > 0.0f);

	if (reproducible) {
		std::cout << "Headless pacing: REPRODUCIBLE (fixed dt " << options.fixedDt
			<< "s). Simulated time is decoupled from wall clock.\n";
	}

	long long ticksRun = 0;
	// Set on the first counted tick, so bootstrap time does not shift the schedule.
	auto countingStarted = std::chrono::steady_clock::now();

	while (true) {
		if (boundedByTicks && ticksRun >= options.runTicks) {
			const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
			std::cout << "Headless run complete after " << ticksRun << " ticks ("
				<< (static_cast<double>(ticksRun) * options.fixedDt) << "s simulated, "
				<< elapsed.count() << "s wall).\n";
			return;
		}

		if (boundedByTime) {
			const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
			if (elapsed.count() >= options.runSeconds) {
				std::cout << "Headless run complete after " << elapsed.count() << "s.\n";
				return;
			}
		}

		// During bootstrap the simulation is not running yet, so those ticks are pure
		// network pumping: they must not be charged to the budget, and spinning
		// through them at full speed would exhaust it before the world exists.
		const bool counting = (!options.countTicksWhen || options.countTicksWhen());

		float dt;
		if (reproducible && counting) {
			// Deliberately ignores the clock. This is what makes the run deterministic:
			// every tick advances the simulation by exactly the same amount regardless
			// of how long it actually took to compute.
			dt = options.fixedDt;
		}
		else {
			timer.Tick();
			dt = timer.GetTimeDeltaSeconds();
			if (dt > 0.1f) {
				// Skip huge deltas (e.g. after a breakpoint), as the game-server loop does.
				continue;
			}
		}

		tick(dt);

		if (counting) {
			if (ticksRun == 0) {
				countingStarted = std::chrono::steady_clock::now();
			}
			++ticksRun;
		}

		if (reproducible && counting) {
			// Pace to the wall clock even though the simulation ignores it.
			//
			// Letting a reproducible run go as fast as the machine allows is wrong in
			// a DISTRIBUTED simulation: each server would advance simulated time at
			// its own rate, so a lightly loaded peer races ahead and exits while a
			// busier one is still handing objects over to it. Those handoffs land on
			// a dead process and the objects are lost outright - conservation breaks
			// for a purely artifactual reason.
			//
			// Pacing each tick to fixedDt of real time keeps every server on the same
			// shared clock, so they stay in step without needing an explicit barrier.
			const auto scheduled = countingStarted +
				std::chrono::duration_cast<std::chrono::steady_clock::duration>(
					std::chrono::duration<double>(static_cast<double>(ticksRun) * options.fixedDt));
			std::this_thread::sleep_until(scheduled);
		}
		else {
			// Cap the spin: ~1 kHz is far above the 60 Hz snapshot cadence.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}
