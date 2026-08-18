#include "MetricSink.h"

#include <fstream>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace NCL {

	uint64_t MonotonicMicros() {
#ifdef _WIN32
		// The performance-counter frequency is fixed at boot, so it is read once.
		static const int64_t frequency = []() {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return f.QuadPart;
			}();

		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);

		// Split the division to keep full resolution without overflowing: the naive
		// (ticks * 1000000) overflows int64 after roughly 30 minutes at a 10 MHz
		// counter, which is well inside a plausible run length.
		const int64_t whole = now.QuadPart / frequency;
		const int64_t rem = now.QuadPart % frequency;
		return static_cast<uint64_t>(whole) * 1000000ull
			+ static_cast<uint64_t>((rem * 1000000ll) / frequency);
#else
		return 0;
#endif
	}

	MetricSink::MetricSink(std::string outputPath, size_t capacity)
		: mOutputPath(std::move(outputPath)) {
		if (mOutputPath.empty()) {
			return;
		}
		// The single allocation for the lifetime of the run.
		mSamples.reserve(capacity);
	}

	void MetricSink::Record(const TickSample& sample) {
		if (mOutputPath.empty()) {
			return;
		}
		// Drop rather than grow: a reallocation here would be attributed to the
		// system under measurement.
		if (mSamples.size() == mSamples.capacity()) {
			++mDropped;
			return;
		}
		mSamples.push_back(sample);
	}

	bool MetricSink::Flush() {
		if (mOutputPath.empty()) {
			return true;
		}

		const bool firstWrite = (mWritten == 0);
		std::ofstream out(mOutputPath, firstWrite ? std::ios::trunc : std::ios::app);
		if (!out) {
			std::cerr << "MetricSink: could not open '" << mOutputPath << "' for writing\n";
			return false;
		}

		if (firstWrite) {
			// Appended at the end, never inserted: analyse.py reads these with
			// csv.DictReader so it keys by name, but any hand-written cut/awk over an
			// existing run would silently shift columns.
			out << "tick,time_us,physics_ms,predict_ms,world_ms,"
				<< "owned_objects,integrated_objects,"
				<< "handoffs_sent,handoffs_received,handoffs_failed,"
				<< "pool_objects,world_objects,forward_entries\n";
		}

		for (size_t i = mWritten; i < mSamples.size(); ++i) {
			const TickSample& s = mSamples[i];
			out << s.tick << ','
				<< s.timeMicros << ','
				<< s.physicsMs << ','
				<< s.predictMs << ','
				<< s.worldMs << ','
				<< s.ownedObjects << ','
				<< s.integratedObjects << ','
				<< s.handoffsSent << ','
				<< s.handoffsReceived << ','
				<< s.handoffsFailed << ','
				<< s.poolObjects << ','
				<< s.worldObjects << ','
				<< s.forwardEntries << '\n';
		}
		mWritten = mSamples.size();

		std::cout << "MetricSink: wrote " << mWritten << " samples to " << mOutputPath;
		if (mDropped > 0) {
			// Loud, because a run that dropped samples is not a valid measurement.
			std::cout << " -- WARNING: DROPPED " << mDropped
				<< " samples (buffer capacity exceeded); this run is not measurement-valid";
		}
		std::cout << std::endl;
		return true;
	}
}
