#include "TaskPool.h"

#include <algorithm>

namespace NCL {

	int TaskPool::DefaultWorkerCount() {
		const unsigned int hardware = std::thread::hardware_concurrency();
		if (hardware == 0) {
			return 0;   // Unknown: stay serial rather than guess.
		}
		// One thread is the caller, which participates. One more is left for the
		// network pump and the OS - a physics loop that saturates every core starves
		// the ENet service and turns into dropped packets, which on this system reads
		// as late halo updates rather than as CPU contention.
		const int usable = static_cast<int>(hardware) - 2;
		return std::max(0, usable);
	}

	TaskPool::TaskPool(int workerCount) {
		if (workerCount <= 0) {
			return;   // Fully serial; ParallelFor runs inline.
		}
		mWorkers.reserve(workerCount);
		for (int i = 0; i < workerCount; ++i) {
			// Worker 0 is the calling thread, so pool workers start at index 1. That
			// keeps the `worker` argument usable directly as a buffer index.
			mWorkers.emplace_back([this, i] { WorkerLoop(i + 1); });
		}
	}

	TaskPool::~TaskPool() {
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mStopping = true;
		}
		mWakeWorkers.notify_all();
		for (std::thread& worker : mWorkers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
	}

	void TaskPool::WorkerLoop(int workerIndex) {
		unsigned long long lastBatch = 0;
		for (;;) {
			{
				std::unique_lock<std::mutex> lock(mMutex);
				mWakeWorkers.wait(lock, [this, lastBatch] {
					return mStopping || mBatchID != lastBatch;
				});
				if (mStopping) {
					return;
				}
				lastBatch = mBatchID;
			}

			// Claim chunks until the batch is exhausted. Claiming rather than being
			// assigned a fixed slice, so a chunk that turns out expensive does not
			// leave the other threads idle - the object cost in a broadphase cell
			// varies with local density, which is exactly the case that would.
			for (;;) {
				const int chunk = mNextChunk.fetch_add(1, std::memory_order_relaxed);
				const int begin = chunk * mChunkSize;
				if (begin >= mCount) {
					break;
				}
				const int end = std::min(begin + mChunkSize, mCount);
				(*mBody)(begin, end, workerIndex);
			}

			if (mActiveWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
				// Last worker out. The lock is taken so the waiting caller cannot be
				// between its predicate check and its wait when this fires.
				std::lock_guard<std::mutex> lock(mMutex);
				mBatchDone.notify_one();
			}
		}
	}

	void TaskPool::ParallelFor(int count,
		const std::function<void(int begin, int end, int worker)>& body) {
		if (count <= 0) {
			return;
		}

		// Serial, and inline on the caller. Not merely an optimisation for small
		// batches: it is what makes the pool safe to use from a unit test or a
		// single-core run without changing any result.
		if (mWorkers.empty()) {
			body(0, count, 0);
			return;
		}

		const int threads = GetParallelism();
		// Several chunks per thread, so the claim loop above can actually balance.
		// One chunk per thread would make claiming pointless.
		constexpr int CHUNKS_PER_THREAD = 4;
		int chunkSize = (count + (threads * CHUNKS_PER_THREAD) - 1) / (threads * CHUNKS_PER_THREAD);
		chunkSize = std::max(1, chunkSize);

		{
			std::lock_guard<std::mutex> lock(mMutex);
			mBody = &body;
			mCount = count;
			mChunkSize = chunkSize;
			mNextChunk.store(0, std::memory_order_relaxed);
			mActiveWorkers.store(static_cast<int>(mWorkers.size()), std::memory_order_relaxed);
			++mBatchID;
		}
		mWakeWorkers.notify_all();

		// The caller takes chunks too, as worker 0. A caller that only waited would
		// leave one core idle for the whole batch.
		for (;;) {
			const int chunk = mNextChunk.fetch_add(1, std::memory_order_relaxed);
			const int begin = chunk * chunkSize;
			if (begin >= count) {
				break;
			}
			const int end = std::min(begin + chunkSize, count);
			body(begin, end, 0);
		}

		std::unique_lock<std::mutex> lock(mMutex);
		mBatchDone.wait(lock, [this] {
			return mActiveWorkers.load(std::memory_order_acquire) == 0;
		});
		mBody = nullptr;
	}
}
