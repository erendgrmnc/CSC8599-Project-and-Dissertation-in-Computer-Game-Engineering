#pragma once
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
}
