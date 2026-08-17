#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NCL {
	// Monotonic high-resolution timestamp, in microseconds since an arbitrary origin.
	//
	// Backed by QueryPerformanceCounter, which on Windows is consistent across
	// processes on the same machine. That is what makes same-machine one-way latency
	// between two roles measurable without any clock-synchronisation protocol -
	// timestamps taken in different processes are directly comparable. Across
	// machines they are NOT comparable and must not be subtracted.
	uint64_t MonotonicMicros();

	// One physics tick on one game server.
	//
	// Deliberately a flat POD: recording must be a couple of stores, and the whole
	// buffer must be writable to disk without walking pointers.
	struct TickSample {
		uint64_t tick = 0;
		uint64_t timeMicros = 0;    // MonotonicMicros() at the end of the tick

		float physicsMs = 0.f;
		float predictMs = 0.f;
		float worldMs = 0.f;

		int32_t ownedObjects = 0;      // objects this server simulates
		int32_t integratedObjects = 0; // objects the integrator actually touched
		int32_t handoffsSent = 0;      // cumulative
		int32_t handoffsReceived = 0;  // cumulative
		int32_t handoffsFailed = 0;    // cumulative

		// Locality (invariant I6). ownedObjects above is what this server SIMULATES;
		// these two are what it HOLDS. Under the pre-seed model every server
		// instantiates the whole world and deactivates what it does not own, so
		// poolObjects and worldObjects are equal to the world total on every server
		// while ownedObjects is only its region's share. The region-local increment is
		// exactly the claim that these two stop scaling with world size, so they have
		// to be recorded to be falsifiable.
		int32_t poolObjects = 0;       // entries in mCreatedObjectPool
		int32_t worldObjects = 0;      // objects in the GameWorld
	};

	// Fixed-capacity, allocation-free per-tick recorder.
	//
	// The @@STAT telemetry line is emitted twice a second and carries a single
	// instantaneous sample, which cannot describe a distribution - it cannot express
	// a p99 tick time, and it hides every spike between samples. This records every
	// tick instead.
	//
	// Two properties matter more than throughput, because this instrument is used to
	// measure the thing it runs inside:
	//
	//  * It NEVER allocates after construction. A reallocation inside a tick would
	//    show up as a latency spike caused by the measurement itself.
	//  * It NEVER writes to stdout. Per-tick logging through the midware's pipe and
	//    the launcher's UI thread would perturb timings by orders of magnitude.
	//
	// If the buffer fills, samples are dropped and counted rather than growing the
	// buffer. A run that dropped samples is not a valid measurement, and the harness
	// is expected to fail it on a non-zero drop count rather than quietly analysing
	// a truncated series.
	class MetricSink {
	public:
		// capacity is in samples; at 120 Hz, 72000 covers a ten-minute run.
		MetricSink(std::string outputPath, size_t capacity);

		void Record(const TickSample& sample);

		// Writes the buffered samples as CSV. Returns false if the file could not be
		// opened. Safe to call more than once; only unwritten samples are emitted.
		bool Flush();

		size_t RecordedCount() const { return mSamples.size(); }
		size_t DroppedCount() const { return mDropped; }
		bool Enabled() const { return !mOutputPath.empty(); }

	protected:
		std::string mOutputPath;
		std::vector<TickSample> mSamples;
		size_t mDropped = 0;
		size_t mWritten = 0;
	};
}
