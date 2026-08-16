#pragma once

// Collision-free network IDs for runtime-spawned objects, without a central
// allocator.
//
// A manager-issued lease was rejected deliberately: SystemManager orchestrates
// bootstrap and then leaves the object data path entirely. Putting allocation there
// would add a round trip to every spawn and make the manager an availability single
// point of failure for gameplay rather than just startup - which would directly
// weaken the "no central physics bottleneck" claim. Static bit partitioning costs
// one shift, needs no round trip, and is provably collision-free.
//
// Deliberately guard-free and dependency-free so the Tier 0 tests can include it.

namespace NCL::NetworkIdSpace {

	// Layout of a 31-bit positive networkID:
	//
	//   bit 30      : 0 = pre-seeded (deterministic, identical on every server)
	//                 1 = runtime-spawned
	//   bits 29..22 : origin server ID   (0..255)
	//   bits 21..0  : per-server counter (0..4,194,303)
	//
	// The pre-seeded range is left EXACTLY as it is today (dense ints from
	// ServerWorldManager's NETWORK_ID_BUFFER), so pre-seeding, handoff and the
	// baseline measurements are untouched by this scheme.
	constexpr int RUNTIME_BIT = 1 << 30;
	constexpr int SERVER_SHIFT = 22;
	constexpr int SERVER_MASK = 0xFF;          // 8 bits -> 256 servers
	constexpr int COUNTER_MASK = (1 << 22) - 1; // 22 bits -> ~4.19M spawns per server

	constexpr int MAX_SERVER_ID = SERVER_MASK;
	constexpr int MAX_COUNTER = COUNTER_MASK;

	inline bool IsRuntimeId(int networkId) {
		return networkId > 0 && (networkId & RUNTIME_BIT) != 0;
	}

	// Returns -1 for a pre-seeded id: those are dense ints with no origin encoded,
	// and claiming otherwise would invent an owner that was never recorded.
	inline int OriginServerOf(int networkId) {
		if (!IsRuntimeId(networkId)) {
			return -1;
		}
		return (networkId >> SERVER_SHIFT) & SERVER_MASK;
	}

	inline int CounterOf(int networkId) {
		if (!IsRuntimeId(networkId)) {
			return -1;
		}
		return networkId & COUNTER_MASK;
	}

	// Returns -1 when the id space is exhausted or the server id is out of range,
	// rather than silently wrapping into another server's space.
	inline int MakeRuntimeId(int serverId, int counter) {
		if (serverId < 0 || serverId > MAX_SERVER_ID) {
			return -1;
		}
		if (counter < 0 || counter > MAX_COUNTER) {
			return -1;
		}
		return RUNTIME_BIT | (serverId << SERVER_SHIFT) | counter;
	}
}
