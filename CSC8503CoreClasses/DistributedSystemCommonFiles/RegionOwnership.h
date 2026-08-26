#pragma once
#include <algorithm>
#include <vector>

#include "Vector3.h"

// The single ownership rule, shared by every role.
//
// It used to be written out three times - ServerWorldManager::IsObjectInBorder,
// ServerWorldManager::GetObjectServer and the client's ResolveCommandTarget - and
// the first two disagreed: one was half-open on X and closed on Z, the other closed
// on both. A point exactly on a shared border was therefore claimed by the handoff
// path and rejected by the pre-seed path, so it could be owned by nobody.
//
// Deliberately free of USEGL / DISTRIBUTEDSYSTEMACTIVE guards and of any engine
// dependency beyond Vector3, so the servers, the client and the Tier 0 tests all
// use this exact code.

namespace NCL::Interaction {

	// One server's slice of the world in XZ. POD, so it can be built from either
	// PhysicsServerBorderData (server side) or ServerRegion (client side).
	struct RegionBounds {
		int   serverId = -1;
		float minX = 0.0f;
		float maxX = 0.0f;
		float minZ = 0.0f;
		float maxZ = 0.0f;
	};

	// Regions are half-open, [minX, maxX) x [minZ, maxZ), so adjacent regions never
	// both claim a shared edge. The world's OUTER maximum is closed instead, or the
	// far edge would belong to no server at all.
	//
	// Returns the owning server id, or -1 if the point lies outside the world.
	// Every point inside the world maps to exactly one server.
	inline int OwningServerFor(const std::vector<RegionBounds>& regions, const Maths::Vector3& point) {
		if (regions.empty()) {
			return -1;
		}

		// Derived from the regions themselves rather than passed in, so a caller
		// cannot supply a world extent that disagrees with the partition.
		float worldMaxX = regions[0].maxX;
		float worldMaxZ = regions[0].maxZ;
		for (const RegionBounds& region : regions) {
			if (region.maxX > worldMaxX) worldMaxX = region.maxX;
			if (region.maxZ > worldMaxZ) worldMaxZ = region.maxZ;
		}

		for (const RegionBounds& region : regions) {
			const bool insideX = point.x >= region.minX &&
				(point.x < region.maxX || (point.x == worldMaxX && region.maxX == worldMaxX));
			const bool insideZ = point.z >= region.minZ &&
				(point.z < region.maxZ || (point.z == worldMaxZ && region.maxZ == worldMaxZ));

			if (insideX && insideZ) {
				return region.serverId;
			}
		}

		return -1;
	}

	// Clamps a point strictly inside `region`, under the SAME rule OwningServerFor
	// applies - so OwningServerFor(regions, ClampIntoRegion(r, ..., p)) == r.serverId.
	//
	// It lives here, next to the rule, deliberately. It used to live in
	// ServerWorldManager as CalculateIncomingObjectOffsetPosition with its own copy of
	// the bounds, and the copy went stale: it clamped Z with an INCLUSIVE upper bound,
	// matching what IsObjectInBorder did before the ownership unification above. On an
	// interior Z seam that returned a coordinate a different server owns - the
	// disowned-object case this file exists to prevent, on the one path that had not
	// been unified. Two definitions of one rule was the defect; one definition is the
	// fix.
	//
	// worldMaxX / worldMaxZ carry the same outer-edge exception OwningServerFor makes:
	// a region on the world boundary owns its maximum, so clamping there must NOT step
	// inward.
	inline Maths::Vector3 ClampIntoRegion(const RegionBounds& region,
		float worldMaxX, float worldMaxZ, const Maths::Vector3& point) {
		// One centimetre in world units - large enough to survive the float rounding
		// that put the object on the edge, far below the 2-unit object spacing.
		constexpr float INWARD_EPSILON = 0.01f;

		// An exclusive upper bound means the maximum itself is not a legal position, so
		// the reachable ceiling is one epsilon below it. A region whose maximum IS the
		// world's owns that maximum, so its ceiling is the maximum itself. std::max
		// guards a degenerate region where the epsilon would invert the range, which
		// would make std::clamp undefined.
		const float ceilingX = (region.maxX == worldMaxX)
			? region.maxX : std::max(region.minX, region.maxX - INWARD_EPSILON);
		const float ceilingZ = (region.maxZ == worldMaxZ)
			? region.maxZ : std::max(region.minZ, region.maxZ - INWARD_EPSILON);

		Maths::Vector3 clamped = point;
		clamped.x = std::clamp(point.x, region.minX, ceilingX);
		clamped.z = std::clamp(point.z, region.minZ, ceilingZ);
		// Y is untouched: the partition is in XZ only.
		return clamped;
	}
}
