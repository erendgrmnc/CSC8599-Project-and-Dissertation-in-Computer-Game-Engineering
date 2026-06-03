#include "HeadlessRunner.h"

#include "GameTimer.h"

#include <thread>
#include <chrono>

void NCL::RunHeadlessLoop(const std::function<void(float dt)>& tick) {
	NCL::GameTimer timer;
	timer.Tick();
	timer.GetTimeDeltaSeconds(); // Clear the timer so we don't get a large first dt.

	while (true) {
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
