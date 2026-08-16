#include "HeadlessRunner.h"

#include "GameTimer.h"

#include <thread>
#include <chrono>
#include <iostream>

void NCL::RunHeadlessLoop(const std::function<void(float dt)>& tick, double runSeconds) {
	NCL::GameTimer timer;
	timer.Tick();
	timer.GetTimeDeltaSeconds(); // Clear the timer so we don't get a large first dt.

	const auto started = std::chrono::steady_clock::now();
	const bool bounded = (runSeconds > 0.0);

	while (true) {
		if (bounded) {
			const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
			if (elapsed.count() >= runSeconds) {
				std::cout << "Headless run complete after " << elapsed.count() << "s.\n";
				return;
			}
		}

		timer.Tick();
		const float dt = timer.GetTimeDeltaSeconds();
		if (dt > 0.1f) {
			// Skip huge deltas (e.g. after a breakpoint), as the game-server loop does.
			continue;
		}

		tick(dt);

		// Cap the spin: ~1 kHz is far above the 60 Hz snapshot cadence.
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
