#pragma once
#include <string>
#include <vector>

namespace NCL {
	// Lightweight command-line flag parser shared by all distributed role entry
	// points (manager / midware / client). Recognizes "--flag value" and bare
	// boolean "--flag" forms. Each role reads the flags it cares about and falls
	// back to the existing interactive std::cin prompts when no flags are present,
	// so launching a role by hand keeps working exactly as before. The GUI
	// launcher constructs these flag strings when it spawns the processes.
	class LaunchConfig {
	public:
		LaunchConfig(int argc, char* argv[]);

		// True when at least one "--flag" token was supplied. Roles use this to
		// decide between flag-driven (non-interactive) and prompt-driven startup.
		bool HasAnyFlags() const;

		bool Has(const std::string& flag) const;
		std::string GetString(const std::string& flag, const std::string& fallback = "") const;
		int GetInt(const std::string& flag, int fallback) const;

	protected:
		std::vector<std::string> mArgs;
		int IndexOf(const std::string& flag) const;
		static bool IsFlag(const std::string& token);
	};
}
