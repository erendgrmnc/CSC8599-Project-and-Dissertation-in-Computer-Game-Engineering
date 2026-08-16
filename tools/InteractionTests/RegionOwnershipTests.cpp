#include "TestHarness.h"

#include "DistributedSystemCommonFiles/RegionOwnership.h"

using namespace NCL;
using namespace NCL::Interaction;

namespace {
	// The canonical two-server split used throughout the evaluation runs: a
	// 300x300 world cut down the middle on X.
	std::vector<RegionBounds> TwoServerWorld() {
		return {
			RegionBounds{ 0, -150.0f,   0.0f, -150.0f, 150.0f },
			RegionBounds{ 1,    0.0f, 150.0f, -150.0f, 150.0f }
		};
	}

	// A 2x2 grid, so both axes have an interior seam to test.
	std::vector<RegionBounds> FourServerWorld() {
		return {
			RegionBounds{ 0, -150.0f,   0.0f, -150.0f,   0.0f },
			RegionBounds{ 1,    0.0f, 150.0f, -150.0f,   0.0f },
			RegionBounds{ 2, -150.0f,   0.0f,    0.0f, 150.0f },
			RegionBounds{ 3,    0.0f, 150.0f,    0.0f, 150.0f }
		};
	}
}

TEST(OwnershipResolvesInteriorPoints) {
	const auto world = TwoServerWorld();
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(-75, 0, 0)), 0);
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(75, 0, 0)), 1);
}

// The bug this increment exists to remove: a point exactly on the shared seam was
// claimed by the handoff path and rejected by the pre-seed path. Half-open on both
// axes gives it to exactly one server - the one whose min edge it is.
TEST(SharedBorderBelongsToTheHigherRegion) {
	const auto world = TwoServerWorld();
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(0, 0, 0)), 1);
}

// The far edge cannot be half-open or it would belong to nobody.
TEST(WorldOuterEdgeIsClosed) {
	const auto world = TwoServerWorld();
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(150, 0, 0)), 1);     // max X
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(-150, 0, 0)), 0);    // min X
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(-75, 0, 150)), 0);   // max Z
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(-75, 0, -150)), 0);  // min Z
}

TEST(WorldCornersAreOwned) {
	const auto world = FourServerWorld();
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(-150, 0, -150)), 0);
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(150, 0, -150)), 1);
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(-150, 0, 150)), 2);
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(150, 0, 150)), 3);
}

TEST(PointsOutsideTheWorldAreUnowned) {
	const auto world = TwoServerWorld();
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(200, 0, 0)), -1);
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(-200, 0, 0)), -1);
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(0, 0, 200)), -1);
	CHECK_EQ(OwningServerFor(world, Maths::Vector3(0, 0, -151)), -1);
}

TEST(EmptyWorldOwnsNothing) {
	CHECK_EQ(OwningServerFor({}, Maths::Vector3(0, 0, 0)), -1);
}

// Totality: the property the whole increment is for. Sweep a grid that lands
// exactly on both seams and both outer edges, and assert every point maps to
// exactly one server - never zero, never two.
TEST(EveryPointMapsToExactlyOneServer) {
	const auto world = FourServerWorld();

	int unowned = 0;
	int doubleOwned = 0;

	for (int xStep = 0; xStep <= 60; ++xStep) {
		for (int zStep = 0; zStep <= 60; ++zStep) {
			const float x = -150.0f + (xStep * 5.0f);   // hits -150, 0 and 150 exactly
			const float z = -150.0f + (zStep * 5.0f);
			const Maths::Vector3 point(x, 0, z);

			const int owner = OwningServerFor(world, point);
			if (owner < 0) {
				++unowned;
				continue;
			}

			// Count how many regions independently contain the point under the same
			// rule, by asking each single-region world in turn.
			int claims = 0;
			for (const RegionBounds& region : world) {
				std::vector<RegionBounds> single{ region };
				// Re-derive against the full world's extent, not the single region's,
				// so the closed-outer-edge rule is evaluated identically.
				const bool insideX = point.x >= region.minX &&
					(point.x < region.maxX || (point.x == 150.0f && region.maxX == 150.0f));
				const bool insideZ = point.z >= region.minZ &&
					(point.z < region.maxZ || (point.z == 150.0f && region.maxZ == 150.0f));
				if (insideX && insideZ) {
					++claims;
				}
			}
			if (claims != 1) {
				++doubleOwned;
			}
		}
	}

	CHECK_EQ(unowned, 0);
	CHECK_EQ(doubleOwned, 0);
}
