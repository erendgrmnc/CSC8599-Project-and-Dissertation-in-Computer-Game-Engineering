#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace NCL {

	// A persistent pool of worker threads, shared by whatever needs parallel work.
	//
	// Why a pool and not std::async or a thread per tick: the physics loop runs at
	// 120 Hz, so anything that creates threads per tick pays thread creation 120 times
	// a second per phase. The workers here are created once and parked on a condition
	// variable between batches.
	//
	// The pool deliberately offers ONLY ParallelFor over an index range, and no general
	// task queue. Every parallel phase in this system is "do the same independent thing
	// to n objects"; a general queue would invite dependencies between tasks, and the
	// one phase that has dependencies - contact resolution - must not be parallelised
	// at all (see below).
	//
	// DETERMINISM. This system's central claim is that two runs of the same
	// configuration produce identical results, so parallelism must not be allowed to
	// change any result. That is only safe where the work is genuinely independent:
	//
	//   safe:   integration, AABB updates, broadphase pair COLLECTION into per-thread
	//           buffers that are merged afterwards in a fixed order.
	//   unsafe: contact resolution. Sequential impulse resolution reads and writes the
	//           velocity of both bodies, so two contacts sharing a body conflict, and
	//           the ORDER contacts are resolved in changes the result. It stays on one
	//           thread.
	//
	// ParallelFor makes that distinction easy to hold: it hands each worker a disjoint
	// index range and never merges anything itself, so a caller who needs a merge has
	// to write it, and writing it is where they are forced to think about order.
	class TaskPool {
	public:
		// 0 workers means "run everything on the calling thread", which is what every
		// unit test and every single-core configuration wants. It is also the fallback
		// whenever the hardware concurrency cannot be determined.
		explicit TaskPool(int workerCount);
		~TaskPool();

		TaskPool(const TaskPool&) = delete;
		TaskPool& operator=(const TaskPool&) = delete;

		// Threads doing work, EXCLUDING the calling thread. 0 means fully serial.
		int GetWorkerCount() const {
			return static_cast<int>(mWorkers.size());
		}

		// Total threads a ParallelFor spreads across: workers plus the caller, which
		// takes a share rather than blocking idle.
		int GetParallelism() const {
			return GetWorkerCount() + 1;
		}

		// Runs body(i) for i in [0, count), split into contiguous ranges.
		//
		// Contiguous rather than strided so each thread walks memory forwards, and so a
		// caller writing into a per-thread buffer knows exactly which indices it owns.
		//
		// Blocks until every index is done. body must not touch anything another index
		// writes; if it must, this is the wrong tool.
		void ParallelFor(int count, const std::function<void(int begin, int end, int worker)>& body);

		// The number of buffers a caller should allocate when collecting per-thread
		// results: one per participating thread, indexed by the `worker` argument.
		int GetBufferCount() const {
			return GetParallelism();
		}

		// Workers to use by default: hardware threads minus one for the calling thread,
		// minus one more left for the network pump and the OS. Never fewer than 0.
		static int DefaultWorkerCount();

	private:
		void WorkerLoop(int workerIndex);

		std::vector<std::thread> mWorkers;
		std::mutex mMutex;
		std::condition_variable mWakeWorkers;
		std::condition_variable mBatchDone;

		// The batch currently being executed. Rebound under mMutex before each batch.
		const std::function<void(int, int, int)>* mBody = nullptr;
		int mCount = 0;
		int mChunkSize = 0;
		// Next chunk index to claim. Atomic so workers take chunks without contending
		// on the mutex once a batch has started.
		std::atomic<int> mNextChunk{ 0 };
		std::atomic<int> mActiveWorkers{ 0 };
		// Incremented per batch, so a worker that wakes spuriously can tell whether the
		// batch it sees is one it has already run.
		unsigned long long mBatchID = 0;
		bool mStopping = false;
	};
}
