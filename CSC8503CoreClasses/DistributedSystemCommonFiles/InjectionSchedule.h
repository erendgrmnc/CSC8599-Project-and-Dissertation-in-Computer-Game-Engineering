#pragma once
#include <cstdint>

#include "InteractionCommand.h"

// The spawn schedule for the AP-comparable injection benchmark.
//
// Aura Projection (Brown, Ushaw & Morgan, I3D 2019) injects 160 objects/second
// for 60 s: 50% into a volume near a region boundary, 50% into one at region
// centre, with types drawn at random and velocities uniform in
// (-10 < x < 10, -10 < y < 0, -10 < z < 10) m/s.
//
// This is a PURE function of (index, seed) with no state, and that is the point:
// every server walks the same schedule and spawns only the objects whose site
// falls inside its own region. No coordination, no central allocator, no
// duplicates - and no server owning the entire population at t=0, which a single
// designated spawner would cause and which would measure a handoff storm rather
// than the benchmark.
namespace NCL::Distributed {

	// AP's published rate. 160/s for 60 s is ~9,600 objects.
	constexpr double AP_INJECTION_RATE = 160.0;

	struct InjectionDraw {
		int index = 0;
		double dueSeconds = 0.0;
		// true = AP's site A, a volume near a region boundary.
		// false = AP's site B, a volume at the region centre.
		bool boundarySite = true;
		int archetypeID = 0;
		// Position within the chosen volume, each in [0, 1). The caller maps these
		// onto the region the draw TARGETS - which it derives from `index`, not from
		// its own server id. Mapping them onto the evaluating server's own region
		// instead makes every server the owner of every draw, and the injection rate
		// comes out multiplied by the server count.
		float u = 0.f, v = 0.f, w = 0.f;
		float velX = 0.f, velY = 0.f, velZ = 0.f;
	};

	namespace Detail {
		// SplitMix64. A counter-based mixer rather than a seeded engine, because an
		// engine carries state and would make a draw depend on how many draws
		// preceded it - which is exactly what must not happen when several servers
		// evaluate the same schedule independently.
		inline uint64_t Mix(uint64_t x) {
			x += 0x9E3779B97F4A7C15ull;
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			return x ^ (x >> 31);
		}

		// [0, 1) from the top 24 bits, which are the best-mixed.
		inline float UnitFloat(uint64_t bits) {
			return static_cast<float>((bits >> 40) & 0xFFFFFFull) / 16777216.0f;
		}

		inline float Draw(uint64_t seed, int index, int stream) {
			const uint64_t key = Mix(static_cast<uint64_t>(seed) * 0x100000000ull
				+ static_cast<uint64_t>(index) * 8ull + static_cast<uint64_t>(stream));
			return UnitFloat(key);
		}
	}

	inline InjectionDraw DrawInjection(int index, uint32_t seed, double ratePerSecond) {
		InjectionDraw draw;
		draw.index = index;
		draw.dueSeconds = (ratePerSecond > 0.0)
			? static_cast<double>(index) / ratePerSecond : 0.0;

		// Site and archetype must NOT share a divisor. Selecting both on index%2
		// would put every sphere at the boundary and every cuboid at the centre -
		// perfectly correlated, where AP draws type independently of site. index%2
		// for the site and (index/2)%2 for the type walks all four combinations in
		// turn, so each site receives an equal mix of both types.
		draw.boundarySite = (index % 2) == 0;
		draw.archetypeID = (((index / 2) % 2) == 0)
			? static_cast<int>(NCL::Interaction::ObjectArchetype::Sphere)
			: static_cast<int>(NCL::Interaction::ObjectArchetype::Cuboid);

		draw.u = Detail::Draw(seed, index, 0);
		draw.v = Detail::Draw(seed, index, 1);
		draw.w = Detail::Draw(seed, index, 2);

		// AP's distribution. y is strictly downward, so injected objects are already
		// falling rather than being lobbed upward.
		draw.velX = Detail::Draw(seed, index, 3) * 20.0f - 10.0f;
		draw.velY = Detail::Draw(seed, index, 4) * -10.0f;
		draw.velZ = Detail::Draw(seed, index, 5) * 20.0f - 10.0f;
		return draw;
	}
}
