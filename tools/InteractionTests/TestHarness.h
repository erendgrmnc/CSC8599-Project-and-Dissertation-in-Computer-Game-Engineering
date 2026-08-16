#pragma once
// Dependency-free test harness. The repo has no test framework and adding one
// would mean a package manager the four role builds do not otherwise need.
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <cmath>

namespace NCL::Testing {

	struct TestCase {
		std::string name;
		std::function<void(int&)> body;
	};

	inline std::vector<TestCase>& Registry() {
		static std::vector<TestCase> registry;
		return registry;
	}

	struct AutoRegister {
		AutoRegister(const std::string& name, std::function<void(int&)> body) {
			Registry().push_back({ name, std::move(body) });
		}
	};

	// Returns the number of FAILED tests, so main can use it as an exit code.
	inline int RunAllTests() {
		int failed = 0;
		for (auto& test : Registry()) {
			int failuresInTest = 0;
			std::cout << "[ RUN      ] " << test.name << "\n";
			test.body(failuresInTest);
			if (failuresInTest == 0) {
				std::cout << "[       OK ] " << test.name << "\n";
			}
			else {
				std::cout << "[  FAILED  ] " << test.name << " (" << failuresInTest << " checks)\n";
				++failed;
			}
		}
		std::cout << "\n" << Registry().size() - failed << " passed, " << failed << " failed.\n";
		return failed;
	}
}

// The int& parameter is the per-test failure counter the CHECK macros increment.
#define TEST(NAME)                                                                    \
	static void NAME(int& claudeTestFailures);                                        \
	static NCL::Testing::AutoRegister claudeAutoRegister_##NAME(#NAME, NAME);         \
	static void NAME(int& claudeTestFailures)

#define CHECK(COND)                                                                   \
	do {                                                                              \
		if (!(COND)) {                                                                \
			++claudeTestFailures;                                                     \
			std::cout << "    FAIL " << __FILE__ << ":" << __LINE__                   \
				<< "  expected: " << #COND << "\n";                                   \
		}                                                                             \
	} while (false)

#define CHECK_EQ(A, B)                                                                \
	do {                                                                              \
		auto claudeLhs = (A);                                                         \
		auto claudeRhs = (B);                                                         \
		if (!(claudeLhs == claudeRhs)) {                                              \
			++claudeTestFailures;                                                     \
			std::cout << "    FAIL " << __FILE__ << ":" << __LINE__                   \
				<< "  " << #A << " == " << #B                                         \
				<< "  (got " << claudeLhs << " vs " << claudeRhs << ")\n";             \
		}                                                                             \
	} while (false)

#define CHECK_NEAR(A, B, TOL)                                                         \
	do {                                                                              \
		const double claudeDiff = std::fabs(double(A) - double(B));                   \
		if (!(claudeDiff <= double(TOL))) {                                           \
			++claudeTestFailures;                                                     \
			std::cout << "    FAIL " << __FILE__ << ":" << __LINE__                   \
				<< "  " << #A << " ~= " << #B                                         \
				<< "  (diff " << claudeDiff << " > " << double(TOL) << ")\n";          \
		}                                                                             \
	} while (false)
