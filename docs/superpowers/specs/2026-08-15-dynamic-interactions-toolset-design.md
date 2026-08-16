# Dynamic Interactions Toolset — design

Date: 2026-08-15
Status: design only, nothing implemented

Adds player-driven movement, runtime spawn/destroy, and force/impulse application to the
distributed physics system, working correctly across server region boundaries.

---

## 0. What the code actually does today (findings that constrain the design)

Everything below was read out of the tree, not assumed. These findings are load-bearing —
several of them invalidate the obvious design.

### 0.1 There is no client→server data path in use

`DistributedPacketSenderServer` already registers `ClientPlayerInputState` and `ClientInit`
(`DistributedGameServerManager.cpp:143-151`), `DistributedGameServerManager::HandleClientPlayerInputPacket`
already routes by `packet->playerID` to `TestObject::ReceiveClientInputs`, and
`TestObject::Update` already turns four booleans into `AddForce` calls. But
**`DistributedMultiplayerGameScene` never sends anything to a physics server** — it registers
only `Delta_State`, `Full_State`, `Player_Connected`, `Player_Disconnected`, `String_Message`
(`DistributedMultiplayerGameScene.cpp:63-69`) and never calls
`GameClient::WriteAndSendClientInputPacket`. The uplink half is scaffolded and dead.

Two defects in that scaffolding to fix rather than build on:
- `HandleClientPlayerInputPacket` writes `mStateIDs[0] = packet->lastId` unconditionally — all
  clients share one acknowledgement slot, so `UpdateMinimumState` prunes state history against
  a single arbitrary client.
- `ClientPlayerInputPacket` sets `size = sizeof(ClientPlayerInputPacket)` rather than
  `sizeof(X) - sizeof(GamePacket)`. `GetTotalSize()` adds `sizeof(GamePacket)` again, so it
  over-sends 4 bytes. `FullPacket`/`DeltaPacket` use the correct form. New packets follow
  `FullPacket`.

### 0.2 The client is already connected to every server, and already knows every region

`SystemManager::SendDistributedPhysicsServerInfoToClients` uses `SendGlobalReliablePacket`
(`SystemManager.cpp:86-90`), so **every** client receives **every** server's
`DistributedClientConnectToPhysicsServer` packet and opens a `GameClient` to each. The client
holds `std::vector<PhysicsServerLink>` keyed by `serverId`, parses each server's
`borderStr` into `mServerRegions`, and maintains `mObjectOwner[objectID] = serverId` on every
snapshot (`ApplyOwnerColour`, `DistributedMultiplayerGameScene.cpp:263-275`).

**The client therefore already has a complete routing table for free.** This is the single
biggest reason to prefer client-side routing over blind forwarding.

### 0.3 Object IDs are dense, deterministic, and pre-seeded — there is no allocator

`ServerWorldManager::AddNetworkObject` hands out `mNetworkIdBuffer++` starting at
`NETWORK_ID_BUFFER = 10` (`ServerWorldManager.cpp:17,116-124`). Every server runs the identical
`CreatePlayerObjects(playerCount, objectsPerPlayer)` → `CreateObjectGrid` and therefore assigns
identical IDs to identical objects. Each server keeps *all* objects in `mCreatedObjectPool` and
activates only those inside its own region.

Consequence: **handoff assumes the target already owns a pool entry for the ID.**
`StartHandlingObject` does `mCreatedObjectPool.at(packet->objectID)` (`ServerWorldManager.cpp:166`)
and `HandleOutgoingObject` does the same (`:213`). An object created at runtime on one server
does not exist in any peer's pool, so **the first border crossing of a spawned object throws
`std::out_of_range` and kills the server process.** Any spawn design must address this before
anything else.

Note also `CreateObjectGrid` picks cube-vs-sphere with `rand() % 2` and no seeding — the shape
of a given ID can differ per server today. Harmless because clients render everything as a cube,
but it means the "identical world on every server" property is *nearly* true, not actually true.

### 0.4 The physics system cannot accept or release objects at runtime

`PhysicsSystem::BroadPhase` populates `mDynamicObjectList` and `mStaticTree` **once**, guarded by
`if (mStaticTree.Empty())` (`PhysicsSystem.cpp:447-459`). After the first tick the list is never
refreshed. `IntegrateAccel` / `IntegrateVelocity` iterate `mDynamicObjectList` only
(`:536,577`). Therefore:

- An object added to `GameWorld` after the first physics tick **is never simulated**.
- An object deleted from `GameWorld` leaves **dangling `GameObject*` in `mDynamicObjectList`,
  `mStaticTree`, and `mAllCollisions`**.
- `ClearForces` calls `o->GetPhysicsObject()->ClearForces()` on every world object with **no null
  check** (`:610-616`) — a spawned object without a `PhysicsObject` crashes the server.

Runtime spawn/destroy is therefore blocked on a `PhysicsSystem` change. This is a prerequisite,
not a detail.

### 0.5 The handoff transfer packet is a broadcast; the ack is directed

`SendFinishTransactionPacket` broadcasts `StartSimulatingObjectPacket` on the packet-sender
server (`DistributedGameServerManager.cpp:466`) — it reaches every peer *and* every game client;
the intended recipient self-selects with `packet->newOwnerServerID == mGameServerID`
(`:219`). The ack goes back over the directed peer mesh:
`SendTransactionHandshakePacket` → `connection->client->SendReliablePacket`
(`:469-480`), landing on the peer's *packet-sender server*, which registered
`StartSimulatingObjectInServerReceived` in `RegisterPacketSenderServerPackets`.

**This asymmetric pattern is the template for every new server↔server message** and must be
reused verbatim: to send A→B directed, use A's `GameClient` for B; B handles it on its
`DistributedPacketSenderServer`.

### 0.6 Handoff is eager and the ack is inert

`HandleObjectTransitions` sends the packet, then immediately calls
`networkObj->HandleTransitionComplete()` and `HandleOutgoingObject()`
(`DistributedGameServerManager.cpp:446-455`). `HandleTransitionComplete` clears
`mNewServerID = -1` (`NetworkObject.cpp:598-602`). `mIsWaitingHandshake` is set but never read.
`HandleTransitionHandshakePacketReceived` has its body commented out (`:312-321`).

Consequence for destroy: **once a handoff is dispatched, the sender has erased the only record
of where the object went** — exactly the information a relayed destroy needs.

### 0.7 Tick ordering is favourable for command application

`ServerStarter.cpp:106-113`:

```cpp
if (serverManager->GetGameStarted()) {
    serverManager->GetServerWorldManager()->Update(dt);   // physics, then CheckPositionOutOfServerBoundaries()
}
serverManager->UpdateGameServerManager(dt);               // network pump, THEN HandleObjectTransitions()
```

So within one tick: transitions are *flagged* → inbound packets are *processed* → transitions are
*dispatched*. A destroy command arriving in the pump is handled before the pending handoff is
sent. This makes "destroy wins over handoff" implementable locally with no protocol change.

Also `TestObject::Update` runs at the top of `ServerWorldManager::Update`, before physics — so
input received in tick *N* affects motion in tick *N+1*. One tick of inherent input latency.

### 0.8 Absence from a snapshot already means "not mine"

`BroadcastSnapshot` skips objects where `!IsNetworkActive()` (`DistributedGameServerManager.cpp:253`),
which is exactly the state `HandleOutgoingObject` puts a handed-off object into. Snapshots are
sent **unreliably** (`GameServer::SendGlobalPacket`, `enet_packet_create(..., 0)`); only handoff
and control packets are reliable.

**Absence from a snapshot stream is already the encoding for "migrated away".** It cannot also
encode "destroyed". This settles §4's explicit-despawn question structurally.

### 0.9 The two border tests disagree, and one returns a dangling reference

```cpp
// ServerWorldManager.cpp:274-282 — half-open on X, CLOSED on Z
bool IsObjectInBorder(pos) { return pos.x >= minX && pos.x < maxX && pos.z >= minZ && pos.z <= maxZ; }

// ServerWorldManager.cpp:284-296 — CLOSED on both, first match in a std::map ordered by server ID
int GetObjectServer(pos) { for (entry : *mServerBorderMap) if (pos.x >= minX && pos.x <= maxX && pos.z >= minZ && pos.z <= maxZ) return entry.first; return -1; }
```

A point exactly on a shared X border is claimed by `GetObjectServer` (lowest server ID wins) but
rejected by `IsObjectInBorder` on that same server. Pre-seeding uses `IsObjectInBorder`; handoff
uses `GetObjectServer`. They can disagree, and the disagreement is precisely the "spawn point on a
border" hazard.

Separately, `CalculateIncomingObjectOffsetPosition` returns `const Maths::Vector3&` bound to a
function-local (`:298-316`) — a dangling reference. It is currently never called, so it is latent.

### 0.10 Packets are memcpy'd structs, and some contain `std::string`

`GameClient::SendPacket` does `enet_packet_create(&payload, payload.GetTotalSize(), 0)` — a raw
byte copy of the C++ object. Several existing packets contain `std::string` /
`std::string[]` members (`DistributedPhysicsClientConnectedToManagerPacket::ipAddress`,
`StartDistributedGameServerPacket::createdServerIPs`, `GameStartStatePacket::levelSeed`). These
work only because short-string optimisation keeps the bytes inline *and* both ends are the same
MSVC x64 binary. The `borderStr[256]` field added later carries an explicit comment saying exactly
this.

**Every new packet in this design is strict POD with fixed-size arrays. No exceptions.**

---

## 1. Authority model

### 1.1 Options considered

| Option | Fit for this architecture |
|---|---|
| **A. Server-authoritative with blind input forwarding** — client sends to any server, that server forwards to the owner | Correct but wasteful. Every command costs an extra hop and an extra serialisation, and the client already has the information needed to avoid it (§0.2). |
| **B. Client-side prediction + server reconciliation** | **Not viable.** Reconciliation requires rolling the local simulation back to an authoritative state and replaying. `PhysicsSystem` has no snapshot/restore. `NetworkObject::stateHistory` stores only position/orientation (`NetworkState`), never velocity, force, or angular state, so a rollback cannot even be *expressed*. `ReadFullPacket`/`ReadDeltaPacket` hard-set the transform with no blending. Building rollback is a larger project than the interaction toolset itself and would put a second simulation in the client, muddying the dissertation's "the servers are the simulation" claim. |
| **C. Handoff-aware client routing, server-authoritative execution** | Zero new client state (§0.2), one hop in the common case, and degrades to A on a stale route. |

### 1.2 Recommendation

**Server-authoritative execution with client-side routing and owner-side forwarding on
misroute.** Stated as an invariant:

> The client's route is a *hint*. Ownership is always resolved server-side, from position, at the
> moment of application. Correctness never depends on the client's routing being right.

That property is what makes the design defensible without prediction: a stale hint costs one
relay hop (≈ one RTT), never a wrong result.

**Player-controlled objects get no special ownership rule.** A player avatar is an ordinary
networked object carrying a `controllerPlayerID`; whichever server currently owns the region
containing it applies that player's input. Introducing a separate "player home server" concept
would fork the handoff protocol into two cases — precisely the thing this design must not do.

### 1.3 Input in flight during a migration

Split commands into two classes, because they need opposite treatment:

**Continuous input (movement axes) is *state*, not an event.** The client re-sends it every client
tick, so a command dropped during migration self-heals within one send interval. It must not be
relayed (a relayed stale axis state is worse than no axis state). To eliminate the one-tick stall
at the moment of handoff, the current axis state travels **inside the handoff payload** — the only
additive change to `StartSimulatingObjectPacket` in this whole design, deferred to increment 7.

**Discrete commands (spawn / destroy / impulse / teleport) are *events*.** They must never be
dropped and must never be applied twice. They are relayed on misroute with a hop limit of 1, and
deduplicated server-side.

Worst case for a discrete command issued exactly as the object migrates:

| t | Event |
|---|---|
| 0 | Client sends `Impulse(obj=42)` to server A (its `mObjectOwner[42] == A`) |
| 0+ε | A dispatches obj 42's handoff to B; A retains `mNewServerID = B` until acked (§6, increment 6) |
| 0+RTT | A receives the impulse, finds obj 42 inactive, sees `mNewServerID == B`, relays to B (hop=1) |
| … | B applies it, acks the *client* directly with `Applied` |

If B has also migrated the object onward, B drops and NACKs `NotOwner` with `correctedServerID`;
the client re-sends once with the corrected hint. Bounded at one retry — no relay chains, no
distributed consensus.

### 1.4 Classes that change

| Class | Change |
|---|---|
| `DistributedMultiplayerGameScene` | Owns the routing table (already has `mServerRegions` + `mObjectOwner`), the command sequence counter, ack handling, and the send path. |
| `DistributedGameServerManager` | New `ReceivePacket` cases that do nothing but look the command up in the registry and invoke it; relay dispatch; dedupe maps; ack emission. |
| `ServerWorldManager` | Implements `ICommandContext` — the only place that touches world/physics state. |
| `TestObject` | Gains `mControllerPlayerID` and an axis-state setter; `ReceiveClientInputs` retained. |
| `NetworkObject` | `CancelPendingTransition()`; `mNewServerID` retained until ack. **No wire-format change until increment 7.** |
| `PhysicsSystem` | `RegisterObject` / `UnregisterObject` (prerequisite, increment 1). |

---

## 2. Command channel

### 2.1 New message types

Appended to `BasicNetworkMessages` in `CSC8503CoreClasses/NetworkBase.h`, **after** the existing
final entry `AddTestObjectsToTheWorld` (`NetworkBase.h:51`). Appending preserves every existing
numeric ID; inserting in the middle silently renumbers the wire protocol for all four roles.

```cpp
	AddTestObjectsToTheWorld,
	//Dynamic Interaction Packet Types
	DistributedClientCommand,        // Client      -> Game Server
	DistributedCommandAck,           // Game Server -> Client
	DistributedServerCommandRelay,   // Game Server -> Game Server
	DistributedObjectSpawned,        // Game Server -> Peers + Clients
	DistributedObjectDespawned       // Game Server -> Peers + Clients
};
```

Five types total. Adding a new *interaction* (grab, teleport, weld, …) adds **zero** message types —
that is the point of the registry in §6.

### 2.2 Packet layouts

Placed in `CSC8503CoreClasses/NetworkObject.h` alongside every other packet, constructors in
`NetworkObject.cpp`. All strict POD, `size = sizeof(X) - sizeof(GamePacket)`.

```cpp
	// Client -> owning game server. One packet shape for every interaction type;
	// the payload is interpreted by the registered IInteractionCommand.
	struct DistributedClientCommandPacket : public GamePacket {
		int commandType;                        // NCL::Interaction::CommandType
		int sequence;                           // per-client monotonic, dedupe + ack key
		int hintServerID;                       // client's belief about the owner, -1 = unknown
		NCL::Interaction::CommandArgs args;     // POD, fixed size

		DistributedClientCommandPacket(int commandType, int sequence, int hintServerID,
			const NCL::Interaction::CommandArgs& args);
	};

	// Game server -> the issuing client. Reliable.
	struct DistributedCommandAckPacket : public GamePacket {
		int sequence;
		int playerID;
		int result;                             // NCL::Interaction::CommandResult
		int correctedServerID;                  // on NotOwner: where it should have gone, else -1

		DistributedCommandAckPacket(int sequence, int playerID, int result, int correctedServerID);
	};

	// Game server -> game server. Carries both the client's identity (so the true
	// owner can ack the client directly) and the relaying server's identity (for the
	// dedupe key on area effects, which fan out to several peers).
	struct DistributedServerCommandRelayPacket : public GamePacket {
		int commandType;
		int originServerID;
		int originSequence;                     // (originServerID, originSequence) = dedupe key
		int hopCount;                           // 0 on send; >0 on receive is a bug, dropped + counted
		int playerID;
		int clientSequence;                     // so the applying server can ack the client
		NCL::Interaction::CommandArgs args;

		DistributedServerCommandRelayPacket(int commandType, int originServerID, int originSequence,
			int playerID, int clientSequence, const NCL::Interaction::CommandArgs& args);
	};
```

Spawn/despawn packets are in §3.3 / §4.3.

Add `static_assert(std::is_trivially_copyable_v<DistributedClientCommandPacket>)` next to each —
this turns a future `std::string` member into a compile error instead of a wire corruption.

### 2.3 Sequence numbers and acking

**Sequencing.** The client keeps one monotonic `mNextCommandSequence`. Every discrete command
carries it. Continuous input reuses the existing `ClientPlayerInputPacket::lastId` path and is
*not* sequenced (it is idempotent state).

**Delivery.** Client→server commands go reliable (`GameClient::SendReliablePacket`), so ENet
already guarantees ordered, exactly-once delivery *on that link*. Reliability does **not** compose
across a relay hop, which is why the ack still matters.

**Acking is for result, not delivery.** `DistributedCommandAckPacket` carries a
`CommandResult` so the client can (a) clear optimistic UI, (b) learn the corrected owner on
`NotOwner`, and (c) feed the I4 accounting invariant in §7. The ack is sent by whichever server
*applied* the command, not by the one that received it.

**Deduplication.** Each server keeps, per player:
- `mLastAppliedSequence[playerID]` — a high-water mark; anything `<=` it is a duplicate.
- a small ring of the last 64 seen sequences, because a relayed command can arrive out of order
  relative to a directly-routed one.

And per relaying server, for area effects: `mSeenRelay[(originServerID, originSequence)]` with the
same ring. Without this, an object inside two overlapping relayed explosion radii receives the
impulse twice.

### 2.4 Routing on a client connected to N servers

`DistributedMultiplayerGameScene::ResolveCommandTarget(const CommandArgs&, const CommandScope&)`:

1. **Object-targeted** (`scope.targetsObject`): look up `mObjectOwner[targetObjectID]` — already
   maintained from every snapshot. Hit → that link.
2. **Point-targeted** (`scope.targetsPoint`): test `worldPoint` against `mServerRegions`, using the
   *same* half-open rule the servers use (§3.4). Hit → that link.
3. **Neither resolves**: reject locally with `CommandResult::Rejected` and log. Do **not** broadcast
   — a broadcast command would be applied N times by N servers, which is the failure mode this
   whole section exists to prevent.

The resolved server ID is also written into `hintServerID` so the server can distinguish "the
client guessed and was wrong" from "the client had no idea" in telemetry.

### 2.5 What happens on a misroute

Server-side, in `IInteractionCommand::Apply` via the context:

```
resolvedOwner = ctx.GetOwningServer(effectivePoint)     // effectivePoint = object position or args.worldPoint
if (resolvedOwner == myServerID)   -> apply, ack Applied
if (resolvedOwner == -1)           -> ack Rejected (outside the world)
if (relayPacket.hopCount > 0)      -> ack NotOwner with correctedServerID, DO NOT relay again
otherwise                          -> relay to resolvedOwner (hopCount = 1), ack Relayed
```

For an object-targeted command where the object is not active here, `effectivePoint` comes from
the object's last known transform in `mCreatedObjectPool` — the pool entry survives handoff, so
even a released object still yields a usable position. If the object is in this server's
`mNewServerID` record (mid-handoff, §0.6), that record is preferred over the positional guess.

**Loop safety** is structural: `hopCount` is 0 or 1 and a relay is never re-relayed. Worst case a
command is dropped and the client retries once with a corrected hint. There is no scenario in
which a command circulates.

### 2.6 Registration points

- `DistributedGameServerManager::RegisterPacketSenderServerPackets()` — add
  `DistributedClientCommand` and `DistributedServerCommandRelay` (peers send relays *to* this
  server's packet-sender server, per §0.5).
- `DistributedGameServerManager::ConnectServerToAnotherGameServer()` — add
  `DistributedObjectSpawned` and `DistributedObjectDespawned` on the outbound peer `GameClient`,
  alongside the existing `StartSimulatingObjectInServer` registration.
- `DistributedMultiplayerGameScene::ConnectClientToDistributedGameServer()` — add
  `DistributedCommandAck`, `DistributedObjectSpawned`, `DistributedObjectDespawned`.

---

## 3. Runtime spawn

### 3.1 Global unique IDs without a central bottleneck

**Recommendation: static high-bit partitioning of the 31-bit positive `int` space. No allocator,
no round trip, no manager involvement.**

New shared header `CSC8503CoreClasses/DistributedSystemCommonFiles/NetworkIdSpace.h`:

```cpp
#pragma once

namespace NCL::NetworkIdSpace {
	// Layout of a 31-bit positive networkID:
	//
	//   bit 30      : 0 = pre-seeded (deterministic, identical on every server)
	//                 1 = runtime-spawned
	//   bits 29..22 : origin server ID   (0..255)
	//   bits 21..0  : per-server counter (0..4,194,303)
	//
	// The pre-seeded range is left exactly as it is today (dense ints from
	// ServerWorldManager's NETWORK_ID_BUFFER = 10) so the existing pre-seeding,
	// handoff and the dissertation's baseline measurements are untouched.
	constexpr int kRuntimeFlag   = 1 << 30;
	constexpr int kServerShift   = 22;
	constexpr int kServerMask    = 0xFF;
	constexpr int kCounterMask   = (1 << 22) - 1;
	constexpr int kMaxRuntimePerServer = kCounterMask;

	inline bool IsRuntimeId(int id)      { return (id & kRuntimeFlag) != 0; }
	inline int  OriginServerOf(int id)   { return IsRuntimeId(id) ? ((id >> kServerShift) & kServerMask) : -1; }
	inline int  MakeRuntimeId(int serverID, int counter) {
		return kRuntimeFlag | ((serverID & kServerMask) << kServerShift) | (counter & kCounterMask);
	}
}
```

`ServerWorldManager` gains `int mRuntimeIdCounter = 0;` and allocates with
`NetworkIdSpace::MakeRuntimeId(mServerID, mRuntimeIdCounter++)`, refusing (and logging + counting)
past `kMaxRuntimePerServer`.

**`OriginServerOf` is for diagnostics only.** Current ownership is *always* positional. Deriving
live ownership from the ID would be wrong the instant the object crosses a border, and would
quietly reintroduce the "player home server" mistake §1.2 rejects.

**Why not a central allocator on the manager.** `SystemManager` is not in the object data path at
all today — it orchestrates bootstrap and then goes quiet. Putting ID allocation there adds a
round trip to every spawn, makes the manager an availability single point of failure for
gameplay, and directly weakens the dissertation's "no central physics bottleneck" claim. The
partition costs one bit-shift and is provably collision-free.

### 3.2 Which server owns a new object

The one whose region contains the spawn point: `ctx.GetOwningServer(args.worldPoint)`. If that is
not the receiving server, the spawn relays like any other command (§2.5) — spawn is not a special
case in the routing layer.

### 3.3 Making the spawn visible to peers and clients

This is where §0.3 bites. `StartHandlingObject` requires the target server to already have a pool
entry, so **every server must learn about a runtime spawn, not just the owner.**

Two mechanisms, and the design uses both:

- **(a) Broadcast `DistributedObjectSpawned` reliably.** The existing
  `mDistributedPacketSenderServer->SendGlobalReliablePacket(...)` reaches peers *and* clients in
  one call — exactly how `StartSimulatingObjectPacket` already reaches peers (§0.5). Peers build
  a **deactivated** pool entry, reproducing the pre-seed model precisely; the owner builds an
  active one; clients build a replica. Handoff then works with **zero changes**.
- **(b) Lazy construction on handoff miss, as a safety net.** `StartHandlingObject` replaces
  `mCreatedObjectPool.at(id)` with a checked `find()`; on a miss it constructs the object from the
  archetype ID carried in the handoff packet rather than throwing.

(a) alone is fragile to a lost broadcast. (b) alone silently changes the meaning of the handoff
protocol — the paper's core contribution — from "reactivate the pre-seeded twin" to "materialise
on demand". Using (a) as the mechanism and (b) as the recovery path keeps the protocol's semantics
intact while removing the crash.

This is why the spawn packet carries an **archetype ID**, not a description: every server must
build a byte-identical object, and passing a description over a memcpy'd struct is exactly the
`std::string`-on-the-wire trap of §0.10.

```cpp
	// Owning game server -> peers (create a deactivated pool entry) and clients
	// (create a replica). Reliable, broadcast on the packet-sender server.
	struct DistributedObjectSpawnedPacket : public GamePacket {
		int objectID;                 // from NetworkIdSpace::MakeRuntimeId
		int archetypeID;              // which prefab; every role builds the same thing
		int ownerServerID;
		int spawnerPlayerID;          // -1 for system spawns
		NetworkState initialState;

		DistributedObjectSpawnedPacket(int objectID, int archetypeID, int ownerServerID,
			int spawnerPlayerID, const NetworkState& initialState);
	};
```

A small `ObjectArchetype` table lives beside `ServerWorldManager::AddCubeToWorld` /
`AddSphereToWorld`, which already are the de-facto prefab factory — it just needs an enum in front
of it and the `rand() % 2` shape choice (§0.3) removed.

### 3.4 Spawn point exactly on a border

Fix the ambiguity at the source rather than special-casing spawn.

**Make `GetObjectServer` the single ownership authority, and define every region as half-open on
both axes**, `[minX, maxX) × [minZ, maxZ)`, with only the world's far edges closed (they already
extend to the world max in `GameInstance::CalculateServerBorders`,
`DistributedPhysicsServerDto.cpp:120-121`, so the closure is a one-line clamp against
`mWorldMaxX` / `mWorldMaxZ`).

Then `IsObjectInBorder` becomes a one-liner:

```cpp
bool ServerWorldManager::IsObjectInBorder(const Maths::Vector3& p) const {
	return GetObjectServer(p) == mServerID;
}
```

Every point in the world now maps to exactly one server, for spawn, handoff, pre-seeding and
destroy alike. A spawn on a border is no longer a special case — it is simply owned by the server
the half-open rule assigns it to.

After ownership is decided, nudge the object a hair inside its owner's region so it is not
re-flagged for handoff on its first tick. That is what
`CalculateIncomingObjectOffsetPosition` was written to do; repair it (fix the dangling
`const Vector3&` return, §0.9) and reuse it rather than adding a parallel mechanism.

### 3.5 Late join / initial state sync

There is no initial sync today — a late client just starts receiving snapshots, and
`HandleFullPacket` calls `SpawnReplica` on first sight of an unknown ID
(`DistributedMultiplayerGameScene.cpp:154-163`). That accidentally works for transforms but loses
the archetype, and can never convey objects that are momentarily not being broadcast.

**Increment 8 design:** on `DistributedPacketSenderServer::AddPeer`, the server emits a
*manifest* — one `DistributedObjectSpawned` per object it currently owns, then a forced full
snapshot. Clients apply manifest entries idempotently by object ID.

**Increment-5 interim:** broadcast the manifest globally rather than to the joining peer. It is
wasteful (every existing client re-receives it) but needs no new plumbing, and existing clients
already dedupe by object ID.

The directed version needs a `GameServer::SendPacketToPeer(int peerNumber, GamePacket&)`, which
does not exist — `GameServer` only has `SendGlobalPacket` / `SendGlobalReliablePacket`, and
`mPeers` is an `int[20]` of peer *numbers*, not `ENetPeer*` handles (`GameServer.cpp:133-147`).
A directed send requires retaining the `ENetPeer*`. Worth doing; not worth blocking spawn on.

---

## 4. Runtime destroy

### 4.1 Explicit despawn packet, not absence from snapshot

**Explicit, and the argument is structural rather than stylistic.**

Absence from a server's snapshot stream *already means* "this server no longer owns the object"
(§0.8) — it is exactly the state handoff produces. The client merges streams from all N servers,
so absence from server A's stream is the normal, expected condition during every migration.
Overloading it with "destroyed" would delete an object from the client on every single border
crossing.

Two further reasons: snapshots are unreliable, and deltas are only meaningful relative to a known
full state — so an inference rule would need a timeout, introducing a visible latency floor and
false-positive deletions under packet loss.

```cpp
	// Owning game server -> peers (drop the pool entry) and clients (drop the replica).
	// Reliable, broadcast on the packet-sender server.
	struct DistributedObjectDespawnedPacket : public GamePacket {
		int objectID;
		int reason;                 // NCL::Interaction::DespawnReason
		int destroyerPlayerID;      // -1 for system despawns

		DistributedObjectDespawnedPacket(int objectID, int reason, int destroyerPlayerID);
	};
```

### 4.2 Ordering against in-flight snapshots

Snapshots are unreliable and can be reordered around the reliable despawn. Without protection, a
late `Full_State` for a destroyed object re-creates it through `SpawnReplica`.

`DistributedMultiplayerGameScene` gains `std::unordered_set<int> mDespawnedObjects;`
`HandleFullPacket` checks it before calling `SpawnReplica`, and `HandleDeltaPacket` before
applying. Counting rejected resurrections gives invariant I3 (§7).

Because IDs are never recycled (§4.4), tombstones can be kept for the run. If memory ever matters,
retire them after a fixed wall-clock window comfortably longer than any plausible reorder.

### 4.3 Destroy racing an in-flight handoff

Four windows, all reachable:

**W1 — destroy arrives after the transition flag is set, before dispatch.**
The tick order (§0.7) guarantees the network pump runs before `HandleObjectTransitions`, so this is
the common case and it is cleanly winnable. **Rule: destroy wins.** Clear the flag, then destroy:

```cpp
	void NetworkObject::CancelPendingTransition();   // mIsActualPosOutServer = false;
	                                                 // mIsWaitingHandshake  = false;
	                                                 // mNewServerID         = -1;
```

A pure local state reset on `NetworkObject`. No wire change.

**W2 — destroy arrives after the object was already sent and released.**
The sender must relay the destroy to the new owner, but `HandleTransitionComplete` has already
wiped `mNewServerID` (§0.6). Change: **retain `mNewServerID` until the ack arrives**, and only then
reset. This is the one place where the *already scaffolded* ack path becomes load-bearing —
`SendTransactionHandshakePacket` and `HandleTransitionHandshakePacketReceived` already exist; the
latter's body is commented out. Completing it is finishing the design that is already in the code,
not replacing it.

**W3 — destroy arrives at the new owner *before* the object does.**
B has no object with that ID. It must not drop the destroy. B records a **pending destroy** keyed
by object ID; when `StartHandlingObject` later runs for that ID, it destroys instead of activating,
and emits the despawn. A server-side tombstone mirroring the client's.

**W4 — two destroys for the same object (one direct, one relayed).**
Destroy is idempotent. The second observes the tombstone and acks `AlreadyDestroyed`. This is why
the dedupe rings of §2.3 exist.

### 4.4 ID reuse policy

**Never reuse. Monotonic counter, no free list.**

Reuse would require distributed agreement on when every server *and* every client has retired an
ID — a distributed garbage-collection problem, which is a research topic of its own and contributes
nothing to this dissertation's thesis. The partitioned space gives 4.19M runtime spawns *per
server*; a dissertation-scale evaluation cannot approach that. Guard exhaustion explicitly: log,
increment a counter, and refuse the spawn with `CommandResult::Rejected`.

Not reusing also makes tombstones permanently safe, which is what makes §4.2 simple.

### 4.5 Local teardown

Do **not** `delete` the `GameObject` on the tick it is destroyed. `PhysicsSystem::mDynamicObjectList`,
`mStaticTree` and `mAllCollisions` all hold raw `GameObject*` (§0.4), and `ClearForces` will
dereference a freed pointer.

Sequence: `SetActive(false)` → `mPhysics->UnregisterObject(obj)` (increment 1) → `GameWorld::RemoveGameObject(obj, false)`
→ push onto `mPendingDeletion` → delete at the *top* of the next tick, after the physics system has
had a frame to drop every reference.

---

## 5. Forces, impulses and cross-border effects

### 5.1 Recommendation: effect relay, not ghost regions

An area effect is a command with an origin, a radius, and a magnitude. The owning server:

1. Applies it to its own active objects inside the radius.
2. Tests the sphere's AABB against every entry in `mServerBorderMap` — already present on every
   server, `N <= 20`, so the cost is trivial.
3. For each overlapped foreign region, sends `DistributedServerCommandRelay` over the existing
   peer mesh (§0.5) with `hopCount = 1`.

Each receiving server applies the effect **only to objects it owns**. No server ever writes to an
object it does not own. Ownership stays exactly where the handoff protocol put it.

### 5.2 Timing and the honest cost

Relayed effects are applied inside the network pump, before `ServerWorldManager::Update` — so the
effect integrates on the same tick locally and on the *next* tick remotely. That one-tick skew
(≈16 ms at the current cadence) is a real cost and should be **measured and reported**, not hidden.

Why it is acceptable: the relayed quantity is an impulse to *velocity*, not a correction to
*position*. A one-tick skew produces a bounded, one-off position error of `|Δv| · dt`. It does not
compound, and it does not diverge — unlike a position correction, which would fight the
simulation every frame.

Use `PhysicsObject::ApplyLinearImpulse` for relayed effects rather than `AddForce`.
`AddForce` accumulates into `mForce`, which `ClearForces` zeroes each tick, so a force relayed with
a one-tick skew lands in a different integration window on each server. An impulse is a direct
velocity change: order-independent and skew-insensitive. It is *not* duplicate-safe, which is what
the `(originServerID, originSequence)` dedupe of §2.3 is for.

### 5.3 Why not ghost regions

Ghost regions — replicating a band of foreign objects into each server's physics world — would
require: duplicated simulation of the band, an authority-arbitration rule for objects inside it,
and doubling the handoff protocol with ghost-enter / ghost-exit events. That is a second
dissertation, and it destabilises the exact protocol this paper is about.

**Be explicit about what the relay does not buy.** It propagates *field* effects — explosions, area
pushes, gravity wells — exactly. It does **not** propagate *contact* effects: a crate shoved into
another crate across a border will not transmit the collision. That is a **pre-existing limitation**
of the system (objects only ever collide within one server's world), not one this design
introduces, and it should be stated as such in the write-up rather than glossed.

---

## 6. The toolset surface

Goal: adding a new interaction type touches **no** network dispatch code. `ReceivePacket` gains its
five cases once and never grows again.

### 6.1 Placement and naming

New files in `CSC8503CoreClasses/DistributedSystemCommonFiles/` — the one directory every role
already compiles, free of renderer/FMOD dependencies and (like the rest of that folder) **not**
wrapped in `#ifndef DISTRIBUTEDSYSTEMACTIVE`.

A new sub-namespace `NCL::Interaction` rather than piling into `NCL::CSC8503`. This matters because
of the documented quirk: `CSC8503CoreClasses/NetworkObject.h:12` carries a **global**
`using namespace NCL::CSC8503;`, so once that header is in a translation unit every CSC8503 name is
visible unqualified. Any new type sharing a name with a CSC8503 type becomes ambiguous at every use
site. Concretely: **do not name anything in `NCL::Interaction` `GameObject`, `Transform`,
`NetworkState`, `NetworkObject`, `GameWorld`, or `PlayerInputs`.** The names below are chosen to
avoid all of them.

### 6.2 `InteractionCommand.h`

```cpp
#pragma once
#include <map>
#include <memory>

#include "Vector3.h"

namespace NCL::CSC8503 { class GameObject; }

namespace NCL::Interaction {

	// Wire values. APPEND ONLY - these travel in DistributedClientCommandPacket::commandType.
	enum class CommandType : int {
		None      = 0,
		MoveAxis  = 1,   // continuous, state-like, not sequenced, not relayed
		Impulse   = 2,
		Spawn     = 3,
		Destroy   = 4,
		Grab      = 5,
		Teleport  = 6
	};

	enum class CommandResult : int {
		Applied = 0,
		Relayed,             // forwarded to the true owner; that server will ack separately
		NotOwner,            // owner changed again; correctedServerID is populated
		ObjectUnknown,
		ObjectDestroyed,
		Duplicate,
		Rejected
	};

	enum class DespawnReason : int { Destroyed = 0, LeftWorld = 1 };

	// POD payload shared by every command type. Fixed size, no std::string /
	// std::vector / pointers: the ENet path memcpys these structs verbatim
	// (GameClient::SendPacket), which is why DistributedClientConnectToPhysicsServerPacket
	// uses a char borderStr[256] rather than a std::string.
	struct CommandArgs {
		int            targetObjectID = -1;   // -1 = none
		int            playerID       = -1;
		int            archetypeID    = 0;    // Spawn: which prefab
		int            flags          = 0;
		Maths::Vector3 worldPoint;            // spawn point / effect origin / teleport destination
		Maths::Vector3 direction;             // impulse or movement axis
		float          magnitude      = 0.0f;
		float          radius         = 0.0f; // > 0 => area effect, may cross region borders
	};

	// Where a command must execute. Derived from the args by the command itself, so
	// the routing layer never switches on CommandType.
	struct CommandScope {
		bool targetsObject = false;  // owner = current owner of targetObjectID
		bool targetsPoint  = false;  // owner = server whose region contains worldPoint
		bool isAreaEffect  = false;  // additionally relay to every region the radius overlaps
		bool isContinuous  = false;  // state, not an event: never sequenced, never relayed
	};

	class ICommandContext;

	class IInteractionCommand {
	public:
		virtual ~IInteractionCommand() = default;

		virtual CommandType  GetType() const = 0;
		virtual CommandScope GetScope(const CommandArgs& args) const = 0;

		// Cheap, side-effect free. Run on the client before routing AND on the
		// server before Apply - the client-side call is an optimisation only.
		virtual bool Validate(const CommandArgs& args) const { return true; }

		// Executed on the authoritative server only, inside the network pump,
		// before ServerWorldManager::Update for that tick.
		virtual CommandResult Apply(ICommandContext& ctx, const CommandArgs& args) = 0;
	};

	// Everything a command is allowed to do. Implemented by ServerWorldManager, so
	// commands never see the network layer and the network layer never sees the world.
	class ICommandContext {
	public:
		virtual ~ICommandContext() = default;

		virtual int GetServerID() const = 0;

		// The single source of truth for ownership. Returns -1 outside the world.
		virtual int GetOwningServer(const Maths::Vector3& worldPoint) const = 0;

		// Active on THIS server only; null if unknown, destroyed, or handed off.
		virtual CSC8503::GameObject* FindActiveObject(int networkObjectID) const = 0;

		// Last known transform for an object in the pool, active or not. Lets a
		// non-owner resolve where an object-targeted command should be relayed.
		virtual bool TryGetLastKnownPosition(int networkObjectID, Maths::Vector3& out) const = 0;

		// Returns the allocated networkID, or -1 on failure (ID space exhausted).
		virtual int  SpawnObject(int archetypeID, const Maths::Vector3& at, int spawnerPlayerID) = 0;
		virtual bool DestroyObject(int networkObjectID, DespawnReason reason, int destroyerPlayerID) = 0;

		virtual void ApplyImpulse(int networkObjectID, const Maths::Vector3& impulse) = 0;
		virtual void ApplyRadialImpulse(const Maths::Vector3& origin, float radius, float magnitude) = 0;
		virtual void SetMoveAxis(int networkObjectID, int playerID, const Maths::Vector3& axis) = 0;

		// Queued, not sent: the manager drains the queue after Apply returns, so a
		// command never blocks inside the network layer.
		virtual void RelayToServer(int serverID, CommandType type, const CommandArgs& args) = 0;

		// Every region the sphere overlaps, excluding this server. Backed by the
		// mServerBorderMap every game server already holds.
		virtual void GetOverlappedServers(const Maths::Vector3& origin, float radius,
			std::vector<int>& outServerIDs) const = 0;
	};

	// One instance per process. Registered once at startup; adding a new interaction
	// type never touches DistributedGameServerManager::ReceivePacket.
	class CommandRegistry {
	public:
		static CommandRegistry& Instance();

		void Register(std::unique_ptr<IInteractionCommand> command);
		IInteractionCommand* Find(CommandType type) const;

		// Registers MoveAxis / Impulse / Spawn / Destroy / Grab / Teleport.
		static void RegisterDefaults();

	protected:
		std::map<CommandType, std::unique_ptr<IInteractionCommand>> mCommands;
	};
}
```

### 6.3 What a concrete command looks like

```cpp
// InteractionCommands.cpp
namespace NCL::Interaction {

	class ImpulseCommand : public IInteractionCommand {
	public:
		CommandType GetType() const override { return CommandType::Impulse; }

		CommandScope GetScope(const CommandArgs& args) const override {
			CommandScope scope;
			scope.targetsObject = (args.targetObjectID >= 0);
			scope.targetsPoint  = (args.targetObjectID <  0);
			scope.isAreaEffect  = (args.radius > 0.0f);
			return scope;
		}

		bool Validate(const CommandArgs& args) const override {
			return args.magnitude > 0.0f && args.magnitude <= kMaxImpulseMagnitude;
		}

		CommandResult Apply(ICommandContext& ctx, const CommandArgs& args) override {
			if (args.radius > 0.0f) {
				ctx.ApplyRadialImpulse(args.worldPoint, args.radius, args.magnitude);

				std::vector<int> overlapped;
				ctx.GetOverlappedServers(args.worldPoint, args.radius, overlapped);
				for (int serverID : overlapped) {
					ctx.RelayToServer(serverID, CommandType::Impulse, args);
				}
				return CommandResult::Applied;
			}

			if (!ctx.FindActiveObject(args.targetObjectID)) {
				return CommandResult::ObjectUnknown;   // caller relays or NACKs
			}
			ctx.ApplyImpulse(args.targetObjectID, args.direction * args.magnitude);
			return CommandResult::Applied;
		}
	};
}
```

`DistributedGameServerManager::ReceivePacket` gains exactly this, once, and never grows:

```cpp
	case BasicNetworkMessages::DistributedClientCommand: {
		auto* packet = static_cast<DistributedClientCommandPacket*>(payload);
		HandleClientCommandPacket(packet, source);
		break;
	}
```

with `HandleClientCommandPacket` performing dedupe → ownership resolution → `registry.Find(type)->Apply(...)`
→ relay drain → ack. Type-agnostic throughout.

### 6.4 New and modified files

**New**

| File | Contents |
|---|---|
| `CSC8503CoreClasses/DistributedSystemCommonFiles/NetworkIdSpace.h` | ID partitioning helpers (header-only). |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h` | The header sketched above. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.cpp` | `CommandRegistry` implementation. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommands.cpp` | The built-in command classes. |
| `DistributedGameServer/ServerCommandContext.h` / `.cpp` | `ICommandContext` implementation bridging `ServerWorldManager` + `DistributedGameServerManager`. |

Register the first four in the `distributed_system_common_files` set of
`CSC8503CoreClasses/CMakePC.cmake` (lines 200-212) and the last in
`DistributedGameServer/CMakePC.cmake`. **Adding to disk is not enough** — the CMake lists are the
build's source of truth.

**Modified**

| File | Change |
|---|---|
| `CSC8503CoreClasses/NetworkBase.h` | Five enum entries appended after `AddTestObjectsToTheWorld`. |
| `CSC8503CoreClasses/NetworkObject.h` / `.cpp` | Five packet structs + constructors; `CancelPendingTransition()`; retain `mNewServerID` until ack; `static_assert`s on layout. |
| `CSC8503CoreClasses/PhysicsSystem.h` / `.cpp` | `RegisterObject` / `UnregisterObject`; incremental `mDynamicObjectList`; null guard in `ClearForces`. |
| `CSC8503CoreClasses/GameServer.h` / `.cpp` | `SendPacketToPeer` + retained `ENetPeer*` (increment 8). |
| `CSC8503CoreClasses/TestObject.h` / `.cpp` | `mControllerPlayerID`; `SetMoveAxis`; keep `ReceiveClientInputs`. |
| `CSC8503CoreClasses/Profiler.h` / `.cpp` | Counters: commands applied/relayed/rejected/duplicate, spawns, despawns, handoff-parity, invariant violations. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/TelemetryReporter.cpp` | Emit the new counters in the `GameServer` and `Client` rows. |
| `DistributedGameServer/ServerWorldManager.h` / `.cpp` | `ICommandContext` impl; spawn/destroy/impulse; runtime ID counter; pending-destroy tombstones; `mPendingDeletion`; checked pool lookups; `IsObjectInBorder` delegates to `GetObjectServer`; repaired `CalculateIncomingObjectOffsetPosition`; archetype table; remove `rand() % 2`. |
| `DistributedGameServer/DistributedGameServerManager.h` / `.cpp` | New handler registrations; five `ReceivePacket` cases; dedupe maps; relay queue drain; ack emission; `HandleObjectTransitions` honours a cancelled transition; per-player `mStateIDs` keying fix. |
| `CSC8503/DistributedMultiplayerGameScene.h` / `.cpp` | Command sending + `ResolveCommandTarget`; sequence counter; ack handling; `mDespawnedObjects` tombstones; tombstone guard in `HandleFullPacket` / `HandleDeltaPacket`; spawn/despawn handlers; `RemoveReplica`. |
| `CSC8503/DistributedClientStart.cpp` | Input capture → command emission. **Lives inside the `#ifndef DISTRIBUTEDSYSTEMACTIVE` block** (mouse/keyboard/raycast are renderer-side); the headless path above it must still build and run with none of it — which is why the scenario driver (§7) sits *outside* the guard. |
| `docs/NETWORKING.md`, `docs/SPATIAL-PARTITIONING.md` | New packet catalogue rows; the positional-ownership rule; the half-open border convention. |

---

## 7. Failure modes and test plan

### 7.1 Races this introduces

| ID | Race | Detection |
|---|---|---|
| R1 | Command applied against a stale owner (client routing lag) | `NotOwner` ack counter in telemetry; a nonzero-but-small rate is normal, a spike means the routing table is not being refreshed |
| R2 | Relay loop A→B→A | `hopCount > 0` on receive is a hard violation; count and log |
| R3 | Command applied twice (direct + relayed, or two overlapping area relays) | Dedupe rings; count duplicate hits. A doubly-applied impulse is visible as 2× velocity |
| R4 | Destroy vs handoff, windows W1–W4 (§4.3) | Invariant I1; explicit counters for "destroy cancelled a pending transition" and "pending destroy consumed on arrival" |
| R5 | Two servers allocate the same runtime ID | Impossible by construction; still assert `OriginServerOf(id) == sender` on receipt and count violations — this catches a mis-set `mServerID`, which is the realistic failure |
| R6 | Spawn broadcast lost → later handoff hits an absent pool entry | Today this **throws** from `.at()`. Replace with `find()` + an `UnknownObjectOnHandoff` counter and the lazy-construct path (§3.3b) |
| R7 | Client resurrects a destroyed object from a reordered snapshot | Tombstone set; count rejected resurrections (I3) |
| R8 | Spawn exactly on a border → claimed by two servers or none | Invariant I1; eliminated by the half-open rule (§3.4), so a nonzero count means the rule regressed |
| R9 | An object active on two servers at once | Invariant I1. **This is latently possible today** given the eager send-and-release handoff; measuring it is valuable independent of this feature |
| R10 | Spawned object never simulated / destroyed object dangling in `mDynamicObjectList` | Assert `mDynamicObjectList.size()` tracks the active count; a spawned object that never moves under gravity is the visible symptom |

### 7.2 Test approach, given there is no test suite

Three tiers, cheapest first. Tier 0 is where most of the value is.

**Tier 0 — pure unit tests, no engine, no network.**
A small `tools/InteractionTests` console executable, deliberately **outside** the CMake role toggle
so it can never perturb the four role builds. Covers:

- `NetworkIdSpace` round-trip and disjointness: for all `(serverID, counter)` samples,
  `OriginServerOf(MakeRuntimeId(s, c)) == s`, no runtime ID collides with a pre-seeded ID, no two
  servers' spaces intersect.
- `GetOwningServer` against a synthetic border map: interior points, exact border points, world
  corners, points outside the world. **Every point maps to exactly one server** is the assertion.
- Routing resolution: given a region map + owner table, `ResolveCommandTarget` picks the expected
  link, including the "owner table is stale" case.
- Dedupe window under out-of-order sequences.
- Packet layout: `static_assert`s that every new packet is trivially copyable and that
  `size + sizeof(GamePacket) == sizeof(T)`.

Fully deterministic, runs in milliseconds, catches R2/R5/R8 and the whole class of memcpy-layout
regressions.

**Tier 1 — deterministic headless scenario.**
The infrastructure mostly exists: `--headless`, `HeadlessRunner`, `@@STAT` telemetry, the
launcher's parser, and per-role exit codes (added in `e0d887d`). Three additions:

1. `--scenario <file>` on the client (parsed by `LaunchConfig`), replaying a command list at fixed
   **tick numbers**, not wall-clock times. Must live outside the `#ifndef DISTRIBUTEDSYSTEMACTIVE`
   guard so it works in a headless client with no renderer.
2. `--fixed-step` on the game server. This is **required** for reproducibility:
   `PhysicsSystem::Update` adapts `realHZ` / `realDT` at runtime based on measured frame cost
   (`PhysicsSystem.cpp:123-141`), so two runs of the same scenario on the same machine do not take
   the same number of physics substeps. Pin `realHZ` and feed a constant `dt`.
3. `--assert-invariants` on the game server: check the invariants below each tick, emit them as
   `@@STAT` keys, and **exit non-zero on violation** so the launcher surfaces the failure.

Canonical scenario: 2 servers, world split at x = 0; spawn 20 objects at x = −10; apply +X impulses
so all 20 cross the border; destroy 5 of them *while they are flagged for transition*; detonate one
radial impulse centred exactly on x = 0.

**Invariants:**

| ID | Invariant |
|---|---|
| I1 | **Exactly one owner.** For every live object ID, the number of servers reporting it active is exactly 1 (0 if destroyed). Each server emits a per-tick digest (`objs=<n> idxor=<xor of active IDs>`); the launcher or a small offline script verifies the per-server ID sets are disjoint and their union is the expected set. No cross-server runtime coordination needed. |
| I2 | **Conservation.** `Σ spawned − Σ destroyed == Σ active`, globally. |
| I3 | **No resurrection.** Client-side count of snapshots accepted for a tombstoned object is 0. |
| I4 | **Command accounting.** `Σ commands sent by clients == Σ (applied + rejected + duplicate-dropped)` across all servers. Catches silently swallowed commands. |
| I5 | **Handoff parity.** `Σ StartSimulatingObjectPacket sent == Σ StartHandlingObject succeeded`. **Add this first, before any interaction work** — it is a baseline regression guard on the untouched handoff protocol, and it is what makes it safe to change the border rule in increment 2. |

**Tier 2 — soak / chaos.**
The same scenario with randomised spawn/destroy/impulse at high rate for ten minutes, asserting
I1–I5 throughout. This is what catches W3 and R6, which are timing-dependent and will not appear in
a scripted run.

**Baseline capture (do this before touching anything).** Record physics time, world time, both
snapshot times, and objects-on-borders for a 2-server and a 4-server run at the current object
counts. The dissertation needs the *delta* attributable to the interaction system, and it cannot be
reconstructed afterwards.

---

## 8. Implementation increments

Each is independently demonstrable and independently revertible.

| # | Increment | Demonstrates | Protocol impact |
|---|---|---|---|
| **0** | **Baseline + safety net.** I5 handoff-parity counters; `mCreatedObjectPool.at()` → checked `find()`; fix the dangling return in `CalculateIncomingObjectOffsetPosition`; null guard in `ClearForces`; per-player `mStateIDs` keying. | Existing runs behave identically; new counters appear in telemetry. | **None.** |
| **1** | **`PhysicsSystem` dynamic registration.** `RegisterObject` / `UnregisterObject`; `mDynamicObjectList` maintained incrementally instead of once. | A server-local test adds a cube after startup and it falls under gravity. | **None.** |
| **2** | **Ownership unification.** `GetObjectServer` becomes the sole authority; half-open on both axes; `IsObjectInBorder` delegates. | Per-server pre-seed counts sum exactly to the total (they may not today, on exact-border rows). | **Semantics only** — changes which server claims an exact-border position. **Gate on increment 0's parity counters.** |
| **3** | **Command channel + `MoveAxis` / `Impulse`.** Enum entries, packet structs, registry, `ICommandContext`, client routing + ack, server dispatch + relay-on-misroute. | Click to push a pre-seeded object — including one owned by a different server than the client last believed. | **Additive.** |
| **4** | **Cross-border area effects.** Radial impulse with region-overlap relay + relay dedupe. | An explosion on the border visibly pushes objects on both sides. | **Additive.** |
| **5** | **Runtime spawn.** ID partitioning, `Spawn` command, `DistributedObjectSpawned` to peers + clients, peer pool-entry creation, archetype table, broadcast manifest. | Spawn an object, watch it cross a border and be handed off correctly. | **Additive.** The handoff *packet* is unchanged; what is new is that peers now create pool entries at runtime rather than only at pre-seed. |
| **6** | **Runtime destroy.** Despawn packet, client + server tombstones, W1–W4 handling, `CancelPendingTransition`, and **finishing the ack path** so `mNewServerID` survives until acknowledged. | Destroy an object mid-handoff; I1/I2 hold. | ⚠️ **The one increment that changes handoff semantics** — the currently-inert ack becomes load-bearing. Requires a dedicated A/B run against increment 0's baseline. |
| **7** | **Player-controlled avatar.** `TestObject` gains a controller ID; continuous input as state; current axis state added to `StartSimulatingObjectPacket`. | Drive an avatar across a border with no input stall. | ⚠️ **The only wire change to the handoff packet.** Additive fields appended at the end of the struct. |
| **8** | **Late-join manifest + directed peer send.** `GameServer::SendPacketToPeer`; per-peer manifest on `AddPeer`. | A client joining a running instance sees the full world immediately, including runtime spawns. | **Additive.** |

### 8.1 Handoff-protocol impact, summarised

The task rightly flags this as the thing not to destabilise. Explicitly:

- **Wire format unchanged** through increments 0–6. `StartSimulatingObjectPacket` and
  `StartSimulatingObjectReceivedPacket` keep their exact layouts. Only increment 7 adds fields, and
  only by appending.
- **Send-and-release eagerness: never changed.** The design does not convert handoff into a
  two-phase commit.
- **Broadcast delivery of the transfer packet: never changed.** New server↔server messages reuse
  the same asymmetric pattern rather than introducing a second one.
- **Pool reactivation: never changed.** Runtime spawns are made to *fit* the pre-seed model
  (peers get deactivated pool entries) rather than the model being changed to accommodate them.
- **Two deliberate semantic changes, each isolated to its own increment and gated on the I5
  baseline:** increment 2 (which server claims an exact-border point) and increment 6 (the ack
  becomes load-bearing). Increment 6 is best framed as *completing* the two-phase design already
  scaffolded in the code — `SendTransactionHandshakePacket`, `mIsWaitingHandshake`,
  `OnTransitionHandshakeReceived` and the commented-out cleanup all exist and are unused — rather
  than as a redesign. That framing is also the honest one.

---

## 9. Open questions

1. **Should `MoveAxis` be a registry command at all?** It is state, not an event, and the existing
   `ClientPlayerInputPacket` path already carries it. Folding it into the registry is conceptually
   tidy but means two wire representations for the same thing. Alternative: leave continuous input
   on `ClientPlayerInputState` and let the registry own only discrete commands. Leaning toward the
   alternative; deferred to increment 3.
2. **Client-side smoothing.** With interaction, snapshot pops become much more noticeable than they
   are for passively falling cubes. Interpolating between the last two full states is orthogonal to
   this design and cheap, but it is *not* prediction and must not be described as such.
3. **Should the manager know about spawns at all?** Currently no — and keeping it out of the data
   path is the right default. But the launcher dashboard would benefit from a global object count,
   which needs either a manager-side aggregation or per-server telemetry summed by the launcher.
   The latter is free (§7's counters) and preserves the no-central-bottleneck property.
