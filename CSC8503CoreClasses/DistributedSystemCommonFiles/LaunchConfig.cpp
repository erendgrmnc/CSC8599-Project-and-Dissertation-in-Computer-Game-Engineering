#include "LaunchConfig.h"

using namespace NCL;

LaunchConfig::LaunchConfig(int argc, char* argv[]) {
	// Skip argv[0] (the executable path); keep everything else verbatim.
	for (int i = 1; i < argc; ++i) {
		mArgs.emplace_back(argv[i]);
	}
}

bool LaunchConfig::IsFlag(const std::string& token) {
	return token.size() > 2 && token[0] == '-' && token[1] == '-';
}

bool LaunchConfig::HasAnyFlags() const {
	for (const std::string& arg : mArgs) {
		if (IsFlag(arg)) {
			return true;
		}
	}
	return false;
}

int LaunchConfig::IndexOf(const std::string& flag) const {
	for (size_t i = 0; i < mArgs.size(); ++i) {
		if (mArgs[i] == flag) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

bool LaunchConfig::Has(const std::string& flag) const {
	return IndexOf(flag) != -1;
}

std::string LaunchConfig::GetString(const std::string& flag, const std::string& fallback) const {
	const int index = IndexOf(flag);
	if (index == -1) {
		return fallback;
	}
	// The value is the next token, provided it is not itself a flag.
	const size_t valueIndex = static_cast<size_t>(index) + 1;
	if (valueIndex >= mArgs.size() || IsFlag(mArgs[valueIndex])) {
		return fallback;
	}
	return mArgs[valueIndex];
}

float LaunchConfig::GetFloat(const std::string& flag, float fallback) const {
	const std::string value = GetString(flag);
	if (value.empty()) {
		return fallback;
	}
	try {
		return std::stof(value);
	}
	catch (...) {
		return fallback;
	}
}

int LaunchConfig::GetInt(const std::string& flag, int fallback) const {
	const std::string value = GetString(flag);
	if (value.empty()) {
		return fallback;
	}
	try {
		return std::stoi(value);
	}
	catch (...) {
		return fallback;
	}
}
