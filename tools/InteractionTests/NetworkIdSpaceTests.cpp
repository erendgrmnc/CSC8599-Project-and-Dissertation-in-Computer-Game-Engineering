#include "TestHarness.h"

#include "DistributedSystemCommonFiles/NetworkIdSpace.h"

using namespace NCL;
using namespace NCL::NetworkIdSpace;

TEST(RuntimeIdRoundTrips) {
	for (int server = 0; server <= 8; ++server) {
		for (int counter : { 0, 1, 2, 999, 65535, MAX_COUNTER }) {
			const int id = MakeRuntimeId(server, counter);
			CHECK(id > 0);
			CHECK(IsRuntimeId(id));
			CHECK_EQ(OriginServerOf(id), server);
			CHECK_EQ(CounterOf(id), counter);
		}
	}
}

// Pre-seeded ids are dense small ints. They must never be mistaken for runtime
// ids, or a handoff would look up an origin server that was never recorded.
TEST(PreSeededIdsAreNotRuntimeIds) {
	for (int id : { 1, 2, 10, 42, 400, 100000 }) {
		CHECK(!IsRuntimeId(id));
		CHECK_EQ(OriginServerOf(id), -1);
		CHECK_EQ(CounterOf(id), -1);
	}
}

// The property the whole scheme exists for: no two servers can ever mint the
// same id, and no runtime id can collide with a pre-seeded one.
TEST(ServerIdSpacesAreDisjoint) {
	for (int a = 0; a <= 6; ++a) {
		for (int b = 0; b <= 6; ++b) {
			if (a == b) {
				continue;
			}
			for (int counter : { 0, 1, 500, MAX_COUNTER }) {
				CHECK(MakeRuntimeId(a, counter) != MakeRuntimeId(b, counter));
			}
		}
	}
}

TEST(RuntimeIdsNeverCollideWithPreSeededIds) {
	// Pre-seeding produces dense ints far below the runtime bit.
	for (int server = 0; server <= 8; ++server) {
		const int lowest = MakeRuntimeId(server, 0);
		CHECK(lowest > 1000000);
	}
}

// Exhaustion and bad input must be reported, not wrapped into a neighbour's
// space - a silent wrap would produce two live objects with one id.
TEST(IdAllocationFailsLoudlyOutOfRange) {
	CHECK_EQ(MakeRuntimeId(-1, 0), -1);
	CHECK_EQ(MakeRuntimeId(MAX_SERVER_ID + 1, 0), -1);
	CHECK_EQ(MakeRuntimeId(0, -1), -1);
	CHECK_EQ(MakeRuntimeId(0, MAX_COUNTER + 1), -1);
}

TEST(RuntimeIdsStayPositive) {
	// Bit 31 must remain clear: ids are passed around as signed ints and a
	// negative id is the failure sentinel everywhere else in the system.
	CHECK(MakeRuntimeId(MAX_SERVER_ID, MAX_COUNTER) > 0);
}
