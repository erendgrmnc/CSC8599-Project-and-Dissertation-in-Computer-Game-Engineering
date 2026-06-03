#pragma once
#include <chrono>

namespace NCL {
	enum class TelemetryRole { Manager, Midware, GameServer, Client };

	// Periodically prints one structured "@@STAT role=... key=val ..." line to
	// stdout, built from the global Profiler metrics relevant to the role. The GUI
	// launcher captures stdout (locally and, for remote machines, forwarded through
	// the midware/agent) and parses these lines into its live status dashboard.
	// Emitted in both headless and windowed modes; rate-limited internally.
	class TelemetryReporter {
	public:
		TelemetryReporter(TelemetryRole role, int id = 0);

		// Call every tick. Emits at most once per interval. gameStarted is only
		// meaningful for GameServer/Client rows (ignored otherwise).
		void MaybeEmit(bool gameStarted = false);

	protected:
		TelemetryRole mRole;
		int mId;
		std::chrono::steady_clock::time_point mLastEmit;
		bool mFirst = true;
	};
}
