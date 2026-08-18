#include "TestHarness.h"

#include "DistributedSystemCommonFiles/TaskPool.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <numeric>
#include <set>
#include <vector>

using namespace NCL;

// A thread pool that drops or repeats work is one of the worst things to debug in a
// system whose central claim is reproducibility: the symptom is a result that is
// almost right, occasionally. These tests pin the properties the physics phases rely
// on - every index exactly once, and no dependence on worker count.

TEST(TaskPoolVisitsEveryIndexExactlyOnce) {
	for (int workers : { 0, 1, 3, 8 }) {
		TaskPool pool(workers);
		constexpr int COUNT = 5000;
		std::vector<std::atomic<int>> visits(COUNT);
		for (auto& v : visits) {
			v.store(0);
		}

		pool.ParallelFor(COUNT, [&](int begin, int end, int) {
			for (int i = begin; i < end; ++i) {
				visits[i].fetch_add(1);
			}
		});

		int wrong = 0;
		for (int i = 0; i < COUNT; ++i) {
			if (visits[i].load() != 1) {
				++wrong;
			}
		}
		CHECK_EQ(wrong, 0);
	}
}

TEST(TaskPoolRangesAreDisjointAndContiguous) {
	// The physics phases rely on ranges being contiguous: each worker walks a block
	// of mDynamicObjectList forwards, and a caller collecting into a per-thread buffer
	// needs to know it owns those indices outright.
	TaskPool pool(4);
	constexpr int COUNT = 1000;

	std::mutex guard;
	std::vector<std::pair<int, int>> ranges;
	pool.ParallelFor(COUNT, [&](int begin, int end, int) {
		std::lock_guard<std::mutex> lock(guard);
		ranges.emplace_back(begin, end);
	});

	std::sort(ranges.begin(), ranges.end());
	int expectedNext = 0;
	for (const auto& range : ranges) {
		CHECK_EQ(range.first, expectedNext);
		CHECK(range.second > range.first);
		expectedNext = range.second;
	}
	CHECK_EQ(expectedNext, COUNT);
}

TEST(TaskPoolWorkerIndexIsWithinBufferCount) {
	// Every parallel phase indexes a per-thread buffer with the worker argument, so an
	// out-of-range value here is an immediate out-of-bounds write in the broadphase.
	TaskPool pool(4);
	const int buffers = pool.GetBufferCount();
	std::atomic<int> outOfRange{ 0 };

	pool.ParallelFor(10000, [&](int, int, int worker) {
		if (worker < 0 || worker >= buffers) {
			outOfRange.fetch_add(1);
		}
	});

	CHECK_EQ(outOfRange.load(), 0);
	CHECK(buffers >= 1);
}

TEST(TaskPoolResultIsIndependentOfWorkerCount) {
	// The property the whole design rests on. The same work split across different
	// numbers of threads must give the same answer, or turning threads on changes the
	// simulation - which would make every reproducibility claim conditional on the
	// machine's core count.
	constexpr int COUNT = 4096;
	std::vector<long long> reference;

	for (int workers : { 0, 1, 2, 5, 9 }) {
		TaskPool pool(workers);
		std::vector<long long> output(COUNT, 0);
		pool.ParallelFor(COUNT, [&](int begin, int end, int) {
			for (int i = begin; i < end; ++i) {
				// Per-index and independent, like integration.
				output[i] = static_cast<long long>(i) * 31 + 7;
			}
		});
		if (reference.empty()) {
			reference = output;
		}
		else {
			CHECK(output == reference);
		}
	}
}

TEST(TaskPoolHandlesEmptyAndSingleWork) {
	// count == 0 happens on a server whose region has emptied; count == 1 happens
	// constantly during bootstrap. Neither should deadlock or run the body wrongly.
	TaskPool pool(4);
	std::atomic<int> calls{ 0 };

	pool.ParallelFor(0, [&](int, int, int) { calls.fetch_add(1); });
	CHECK_EQ(calls.load(), 0);

	std::atomic<int> indices{ 0 };
	pool.ParallelFor(1, [&](int begin, int end, int) {
		for (int i = begin; i < end; ++i) {
			indices.fetch_add(1);
		}
	});
	CHECK_EQ(indices.load(), 1);
}

TEST(TaskPoolRunsManyBatchesWithoutLosingWork) {
	// The workers park on a condition variable between batches, so a missed wake-up
	// would show as a batch that silently does less work. Many small batches is the
	// shape the physics loop actually produces - two per substep, 120 times a second.
	TaskPool pool(4);
	constexpr int BATCHES = 500;
	constexpr int COUNT = 64;
	std::atomic<long long> total{ 0 };

	for (int batch = 0; batch < BATCHES; ++batch) {
		pool.ParallelFor(COUNT, [&](int begin, int end, int) {
			total.fetch_add(end - begin);
		});
	}

	CHECK_EQ(total.load(), static_cast<long long>(BATCHES) * COUNT);
}

TEST(TaskPoolWithZeroWorkersRunsOnTheCallingThread) {
	// The configuration every unit test and every single-core run uses. It must not
	// create threads at all, so a serial build behaves exactly as it did before the
	// pool existed.
	TaskPool pool(0);
	CHECK_EQ(pool.GetWorkerCount(), 0);
	CHECK_EQ(pool.GetParallelism(), 1);

	const std::thread::id caller = std::this_thread::get_id();
	bool ranElsewhere = false;
	pool.ParallelFor(100, [&](int, int, int) {
		if (std::this_thread::get_id() != caller) {
			ranElsewhere = true;
		}
	});
	CHECK(!ranElsewhere);
}
