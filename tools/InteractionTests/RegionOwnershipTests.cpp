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

// --- ClampIntoRegion ------------------------------------------------------------
//
// The arrival clamp must agree with the rule above, not with a stale description of
// it. Until 2026-08-26 ServerWorldManager::CalculateIncomingObjectOffsetPosition kept
// its own copy of the bounds and clamped Z with an INCLUSIVE upper bound, matching what
// IsObjectInBorder did BEFORE the ownership unification. OwningServerFor is half-open on
// both axes, so on an interior Z seam that clamp returned a coordinate a DIFFERENT
// server owns - the disowned-object case the unification exists to prevent, on the one
// path that had not been unified.
//
// Masked at 2 servers (1-D split, maxZ == worldMaxZ, so the closed-outer-edge exception
// applies) and live at 4. That is why no run caught it: every conservation measurement
// before 2026-08-26 ran at 2 servers.

TEST(ClampedPointOnAnInteriorZSeamIsOwnedByThisServer) {
	// THE REGRESSION TEST. Server 0 owns [-150,0) x [-150,0). An object arriving at
	// z = 0 sits exactly on the interior seam; the old clamp returned z = 0 unchanged,
	// and OwningServerFor gives z = 0 to server 2.
	const auto world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(-10, 0, 0));
	CHECK_EQ(OwningServerFor(world, clamped), 0);
}

TEST(ClampedPointPastAnInteriorZSeamIsOwnedByThisServer) {
	const auto world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(-10, 0, 7.5f));
	CHECK_EQ(OwningServerFor(world, clamped), 0);
}

TEST(ClampedPointOnTheWorldOuterZEdgeStaysOnIt) {
	// The outer maximum is CLOSED - it belongs to the last row, or it would belong to
	// nobody. Clamping it inward would be wrong, not merely unnecessary.
	const auto world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[2], 150.0f, 150.0f, Maths::Vector3(-10, 0, 150));
	CHECK_EQ(clamped.z, 150.0f);
	CHECK_EQ(OwningServerFor(world, clamped), 2);
}

TEST(ClampedPointOnAnInteriorXSeamIsOwnedByThisServer) {
	// X was already correct. Pinned so the epsilon treatment is not lost while Z gains it.
	const auto world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(0, 0, -10));
	CHECK_EQ(OwningServerFor(world, clamped), 0);
}

TEST(ClampIsANoOpForAPointAlreadyInside) {
	const auto world = FourServerWorld();
	const Maths::Vector3 inside(-75, 3, -75);
	const Maths::Vector3 clamped = ClampIntoRegion(world[0], 150.0f, 150.0f, inside);
	CHECK_EQ(clamped.x, inside.x);
	CHECK_EQ(clamped.y, inside.y);
	CHECK_EQ(clamped.z, inside.z);
}

TEST(ClampPreservesTheYCoordinate) {
	// The clamp is an XZ operation. A handoff that silently floored Y would drop
	// objects through the world.
	const auto world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(0, 42, 0));
	CHECK_EQ(clamped.y, 42.0f);
}

TEST(TwoServerSplitIsUnaffectedByTheZFix) {
	// The 1-D case must not move: every conservation figure on record was measured on
	// it, and Gate A compares against those runs.
	const auto world = TwoServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(-10, 0, 150));
	CHECK_EQ(clamped.z, 150.0f);
	CHECK_EQ(OwningServerFor(world, clamped), 0);
}
