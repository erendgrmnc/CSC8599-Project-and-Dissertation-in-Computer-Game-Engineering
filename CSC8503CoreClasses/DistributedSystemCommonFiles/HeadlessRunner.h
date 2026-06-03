#pragma once
#include <functional>

namespace NCL {
	// Runs a GameTimer-paced loop calling tick(dt) until the process is terminated
	// externally (the launcher taskkills role processes). Used by the distributed
	// roles in --headless mode, where there is no Window / ProfilerRenderer to gate
	// or time the loop. A short sleep caps CPU spin while staying well above the
	// 60 Hz snapshot rate, and oversized deltas (e.g. after a breakpoint) are skipped
	// to match the windowed game-server loop's behaviour.
	void RunHeadlessLoop(const std::function<void(float dt)>& tick);
}
