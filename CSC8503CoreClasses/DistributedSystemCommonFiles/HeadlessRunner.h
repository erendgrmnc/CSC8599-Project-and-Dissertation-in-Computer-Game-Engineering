#pragma once
#include <functional>

namespace NCL {
	// Runs a GameTimer-paced loop calling tick(dt). Used by the distributed roles in
	// --headless mode, where there is no Window / ProfilerRenderer to gate or time
	// the loop. A short sleep caps CPU spin while staying well above the 60 Hz
	// snapshot rate, and oversized deltas (e.g. after a breakpoint) are skipped to
	// match the windowed game-server loop's behaviour.
	//
	// runSeconds <= 0 runs until the process is terminated externally, which is the
	// original behaviour. A positive value returns cleanly after that long, which is
	// what unattended measurement runs need: a fixed, comparable duration, and a
	// chance to flush recorded metrics rather than being force-killed with the
	// buffer still in memory.
	void RunHeadlessLoop(const std::function<void(float dt)>& tick, double runSeconds = 0.0);
}
