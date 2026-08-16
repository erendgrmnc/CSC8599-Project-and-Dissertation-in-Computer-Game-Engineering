#pragma once
#include <array>

namespace NCL {

	// Duplicate detection for client commands.
	//
	// Client -> server is reliable, so ENet already guarantees exactly-once on THAT
	// link. Reliability does not compose across a relay hop: a relayed command is a
	// separate reliable send on a different link with no ordering relationship to the
	// first, so the same command can arrive twice and out of order. Hence a high-water
	// mark plus a small ring of recently seen sequences, not just a counter.
	class SequenceWindow {
	public:
		static constexpr int WINDOW_SIZE = 64;

		// True if this sequence has not been seen before and is recent enough to
		// judge. False means "duplicate, or too old to be sure" - both are dropped.
		bool Accept(int sequence) {
			if (sequence <= mHighWater - WINDOW_SIZE) {
				return false;   // Older than anything we still remember.
			}

			if (sequence > mHighWater) {
				mHighWater = sequence;
				mSeen[Slot(sequence)] = sequence;
				return true;
			}

			if (mSeen[Slot(sequence)] == sequence) {
				return false;   // Already applied.
			}

			mSeen[Slot(sequence)] = sequence;
			return true;
		}

		int GetHighWater() const {
			return mHighWater;
		}

	private:
		static int Slot(int sequence) {
			return ((sequence % WINDOW_SIZE) + WINDOW_SIZE) % WINDOW_SIZE;
		}

		// Sentinel below every legal sequence; sequences start at 1.
		int mHighWater = 0;
		std::array<int, WINDOW_SIZE> mSeen{};
	};
}
