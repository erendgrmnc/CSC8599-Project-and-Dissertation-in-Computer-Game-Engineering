# Dynamic Interactions Toolset — Design

Date: 2026-08-16
Status: **design only — nothing implemented**
Supersedes: `2026-08-15-dynamic-interactions-toolset-design.md` (same subject; this revision adds
build-guard constraints, the prediction-path coupling, and a tightened increment order)

Goal: add player-driven interaction — movement commands, runtime spawn, runtime destroy, and
forces/impulses that cross region boundaries — to the distributed physics system, **without
destabilising the predictive handoff protocol that is the paper's core contribution.**

---

## 0. Verified ground truth

Every claim below was read out of the tree at the line numbers given. They are load-bearing:
several of them invalidate the obvious design, and three of them are hard blockers.

### 0.1 Objects are created once, identically, on every server

`ServerWorldManager::CreatePlayerObjects` → `CreateObjectGrid`
(`ServerWorldManager.cpp:126-141`, `:222-264`). **Every server builds the identical full object
set** and deactivates the ones outside its own region (`:249-255`). `mCreatedObjectPool`
(`ServerWorldManager.h:60`) is therefore pre-seeded with *all* objects on *all* servers, and
handoff is "reactivate a pooled entry" — never instantiation.

IDs come from a per-server monotonic counter `mNetworkIdBuffer` starting at `NETWORK_ID_BUFFER = 10`
(`:17`, `:31`, `:116-124`). Servers agree **only** because they run identical deterministic
construction. There is no authority, no ID-range partitioning, and no reservation protocol.

`CreateObjectGrid` picks cube-vs-sphere with `rand() % 2` (`:237`) and **there is no `srand` call
anywhere in the tree**, so every process gets libc's default sequence and the agreement holds by
accident. It is currently harmless (clients render everything as a cube) but it means "identical
world on every server" is *nearly* true, not actually true — and it is the first thing to fix for
reproducible measurements (§7).

**Any runtime spawn breaks ID agreement immediately.** That is the problem §3 exists to solve.

### 0.2 BLOCKER — the physics system cannot accept objects at runtime

`PhysicsSystem::BroadPhase` populates `mDynamicObjectList` and `mStaticTree` exactly once, guarded
by `if (mStaticTree.Empty())` (`PhysicsSystem.cpp:447-459`). The floor is inserted in
`ServerWorldManager`'s constructor (`:36`), so the tree is non-empty from the first tick onward and
the list is **never refreshed**.

Consequences, all confirmed:

- `IntegrateAccel` (`:536`) and `IntegrateVelocity` (`:577`) iterate `mDynamicObjectList` only.
  **An object added after the first physics tick is never simulated.**
- `PredictFuturePositions` (`:213-229`) *also* iterates `mDynamicObjectList`. This is the
  under-appreciated half: a runtime-spawned object would not merely fail to fall, it would never
  get a predicted position, so it would **never be handed off** — it would silently fall out of the
  predictive protocol entirely.
- Removing an object from `GameWorld` leaves dangling `GameObject*` in `mDynamicObjectList`,
  `mStaticTree`, `mBroadphaseCollisions` and `mAllCollisions`. `UpdateCollisionList` (`:155-174`)
  dereferences those pointers to call `OnCollisionEnd` for up to `mNumCollisionFrames` frames
  after the fact — a use-after-free.
- `ClearForces` (`:610-616`) calls `o->GetPhysicsObject()->ClearForces()` on **every** world object
  with no null check, unlike `IntegrateAccel` which does guard. Any object without a
  `PhysicsObject` crashes the server.

**Runtime spawn and destroy are blocked on a `PhysicsSystem` change. This is a prerequisite, not a
detail.** It is increment 1.

### 0.3 BLOCKER — the pool lookup throws on unknown IDs

`StartHandlingObject` does `mCreatedObjectPool.at(packet->objectID)`
(`ServerWorldManager.cpp:166`) and `HandleOutgoingObject` does the same (`:213`).
`std::map::at` **throws `std::out_of_range`** on a miss, and nothing catches it. A spawned object
that exists on one server but not its peer therefore kills the peer process on its first border
crossing.

Note also that `at()` returning a valid pointer is the `if` condition — a null value in the map
would fall through to `return false`, but a *missing key* throws. This must become a checked
`find()` before any spawn work begins.

### 0.4 No spawn/destroy protocol exists

There are no spawn or destroy packets. The enum entry `AddTestObjectsToTheWorld`
(`NetworkBase.h:51`) is declared and **never referenced anywhere**. `GameWorld::RemoveGameObject`
exists (`GameWorld.cpp:51-57`, defaulting to `andDelete = false`) but is never called by any
distributed code. "Removal" throughout the system is `SetActive(false)`.

### 0.5 The client is passive, but the server-side input path is complete and wired

`GameClient::WriteAndSendClientInputPacket` (`GameClient.cpp:87-91`) exists and is **never called
from anywhere**. `DistributedMultiplayerGameScene` registers only `Delta_State`, `Full_State`,
`Player_Connected`, `Player_Disconnected`, `String_Message`
(`DistributedMultiplayerGameScene.cpp:64-68`) and never sends to a physics server.

But the receiving half is fully built:

| Step | Location |
|---|---|
| `ClientPlayerInputPacket` struct | `NetworkObject.h:75-83` |
| Handler registered on the packet-sender server | `DistributedGameServerManager.cpp:144` |
| Dispatched in `ReceivePacket` | `:205-209` |
| `HandleClientPlayerInputPacket` routes by `playerID` | `:176-191` |
| `TestObject::ReceiveClientInputs` stores 4 booleans | `TestObject.cpp:46-51` |
| `TestObject::Update` turns them into ±50 `AddForce` calls | `TestObject.cpp:23-44`, `:64-78` |

**The client→server command channel is a short hop, not a greenfield build.**

Two defects in that scaffolding to repair rather than build on:
- `HandleClientPlayerInputPacket` writes `mStateIDs[0] = ...` unconditionally (`:183`) — every
  client shares one acknowledgement slot, so `UpdateMinimumState` prunes state history against a
  single arbitrary client.
- `ClientPlayerInputPacket`'s constructors set `size = sizeof(ClientPlayerInputPacket)`
  (`NetworkObject.cpp:45`, `:78`) rather than `sizeof(X) - sizeof(GamePacket)`. `GetTotalSize()`
  adds `sizeof(GamePacket)` again, so the send over-reads 4 bytes past the struct. This is **not
  isolated** — `StartSimulatingObjectPacket` (`:335`), `StartSimulatingObjectReceivedPacket`
  (`:361`), `RunDistributedPhysicsServerInstancePacket` (`:370`) and the rest of the distributed
  packets all do it. Only `FullPacket`/`DeltaPacket`/`ClientPacket` (`NetworkObject.h:24,37,48`)
  use the correct form. **New packets follow `FullPacket`.** Each packet is self-describing, so
  mixing conventions is safe; fixing the existing ones changes wire sizes and is deliberately *not*
  part of this design.

### 0.6 The client already has a complete routing table, for free

`SystemManager::SendDistributedPhysicsServerInfoToClients` broadcasts, so **every client receives
every server's** `DistributedClientConnectToPhysicsServer` packet and opens a separate `GameClient`
to each (`DistributedMultiplayerGameScene.cpp:184-213`). The client holds:

- `mDistributedPhysicsClients` — `{GameClient*, serverId}` pairs (`:71`),
- `mServerRegions` — every server's parsed region rectangle (`:201-206`),
- `mObjectOwner[objectID] = serverId` — refreshed on **every** snapshot, via `ApplyOwnerColour`
  called from both `HandleFullPacket` and `HandleDeltaPacket` (`:154-171`, `:263-275`),
- `mActiveServerId` — which link is currently being pumped (`:176-182`), which is what attributes
  each snapshot to its sender.

**This is the single biggest reason to prefer client-side routing over blind forwarding**: the
routing table already exists and is already maintained.

### 0.7 Handoff: eager send, dead ack, and an ownership gap

`HandleObjectTransitions` (`DistributedGameServerManager.cpp:446-455`) sends the packet, then
**immediately** calls `HandleTransitionComplete()` and `HandleOutgoingObject()` — no waiting for an
acknowledgement. The ack path is stubbed dead:

- `ServerWorldManager::HandleTransitionHandshakeReceived` — empty body (`ServerWorldManager.cpp:207-210`)
- `HandleTransitionHandshakePacketReceived` — loop present, body commented out
  (`DistributedGameServerManager.cpp:312-321`)
- `NetworkObject::OnTransitionHandshakeReceived` — never called
- `mIsWaitingHandshake` — set but never read

So there is a genuine **ownership gap**: between send and receive, nobody owns the object and
nobody broadcasts it. There is no rollback — if the packet is lost the object is gone. And once
the handoff is dispatched, `HandleTransitionComplete` clears `mNewServerID` (`NetworkObject.cpp`),
**erasing the only record of where the object went** — which is exactly the information a relayed
destroy needs (§4.3).

### 0.8 The transfer is a broadcast; the ack is directed

`SendFinishTransactionPacket` uses `mDistributedPacketSenderServer->SendGlobalReliablePacket`
(`:466`) — reaching every peer **and every game client**, which log "Received unknown packet"
(`DistributedMultiplayerGameScene.cpp:104-106`). The intended recipient self-selects with
`packet->newOwnerServerID == mGameServerID` (`:219`).

The ack goes the other way: `SendTransactionHandshakePacket` uses `connection->client->SendReliablePacket`
(`:469-480`) over the directed peer mesh, landing on the peer's *packet-sender server*, which
registered `StartSimulatingObjectInServerReceived` in `RegisterPacketSenderServerPackets` (`:147`).

Note the asymmetry carefully, because it is easy to get wrong: a peer receives the **broadcast**
handoff on its **outbound `GameClient`** (registered in `ConnectServerToAnotherGameServer`,
`:491`), but receives the **directed ack** on its **`DistributedPacketSenderServer`** (`:147`).

**This is the template for every new server↔server message and must be reused verbatim.**

### 0.9 Tick ordering is favourable for command application

`ServerStarter.cpp:106-113`:

```cpp
auto tick = [&](float dt) {
    if (serverManager->GetGameStarted()) {
        serverManager->GetServerWorldManager()->Update(dt);   // TestObject::Update, physics,
    }                                                         // then CheckPositionOutOfServerBoundaries()
    serverManager->UpdateGameServerManager(dt);               // network pump (:90-100),
    ...                                                       // THEN HandleObjectTransitions() (:103)
};
```

Within one tick: transitions are **flagged** → inbound packets are **processed** → transitions are
**dispatched**. A destroy command arriving in the pump is therefore handled *before* the pending
handoff is sent. **"Destroy wins over handoff" is implementable locally with no protocol change.**

Also, `TestObject::Update` runs at the top of `ServerWorldManager::Update` (`:83-92`), before
physics — so input received in tick *N* affects motion in tick *N+1*. One tick of inherent input
latency, unavoidable without reordering the loop.

### 0.10 Absence from a snapshot already means "not mine"

`BroadcastSnapshot` skips objects where `!IsNetworkActive()` (`:251-255`) — exactly the state
`HandleOutgoingObject` leaves a handed-off object in. Snapshots go out via `SendGlobalPacket`
(unreliable, `enet_packet_create(..., 0)`); only handoff and control packets are reliable.

**Absence from a snapshot stream is already the encoding for "migrated away". It cannot also encode
"destroyed".** This settles §4.1 structurally, not stylistically.

### 0.11 The two border tests disagree

```cpp
// ServerWorldManager.cpp:274-282 — half-open on X, CLOSED on Z
bool IsObjectInBorder(pos) {
    return pos.x >= minX && pos.x <  maxX
        && pos.z >= minZ && pos.z <= maxZ;
}

// ServerWorldManager.cpp:284-296 — CLOSED on both axes; first match in a
// std::map ordered by server ID, so the lowest server ID wins
int GetObjectServer(pos) {
    for (entry : *mServerBorderMap)
        if (pos.x >= minX && pos.x <= maxX && pos.z >= minZ && pos.z <= maxZ)
            return entry.first;
    return -1;
}
```

A point exactly on a shared X border is claimed by `GetObjectServer` but **rejected** by
`IsObjectInBorder` on that same server. Pre-seeding uses `IsObjectInBorder` (`:249`); handoff uses
`GetObjectServer` (`:154`). They can disagree, and the disagreement *is* the "spawn point on a
border" hazard. This is an existing latent bug and the design must not inherit it.

Separately, `CalculateIncomingObjectOffsetPosition` (`:298-316`) returns `const Maths::Vector3&`
bound to a function-local — a dangling reference. It is currently never called, so it is latent.

### 0.12 Build guards: `#ifdef USEGL` wraps the networking layer

**Not previously documented, and it constrains where new code can live.** `NetworkBase.h`,
`NetworkObject.h` and `GameClient.cpp` are each wrapped **in their entirety** in `#ifdef USEGL`
(`NetworkBase.h:1`/`:151`, `NetworkObject.h:2`/`:388`, `GameClient.cpp:1`/`:182`).

This is separate from, and stacks with, the `#ifndef DISTRIBUTEDSYSTEMACTIVE` demo-only guard
described in CLAUDE.md. Consequences for this design:

- New packet structs go inside the existing `USEGL` block in `NetworkObject.h`. Fine — that is
  where every other packet lives.
- **New toolset headers must not be `USEGL`-guarded**, because the Tier-0 unit tests (§7.3) need to
  include them without the engine. `InteractionCommand.h` is therefore pure C++ over `Vector3` with
  no `NetworkBase.h` include, and the *packet* that carries a `CommandArgs` lives in
  `NetworkObject.h` behind the guard.

### 0.13 Other confirmed hazards

- `GameServer::mPeers = new int[20]` fixed array (`GameServer.cpp:12`), but the loop that fills it
  runs to `mClientMax` (`:13-15`) and `SetMaxClients` (`:125-127`) can raise `mClientMax` freely.
  `DistributedGameServerManager` computes `maxClient = (totalServerCount - 1) + clientsToConnect`
  (`:397`) and calls `SetMaxClients(maxClient)` (`:400`). **Past 20 total peers this is a heap
  overflow** in both the constructor loop and `AddPeer` (`:133-147`). Any evaluation run with
  servers + clients > 20 is already corrupt.
- `GameServer` exposes only `SendGlobalPacket` / `SendGlobalReliablePacket`. **There is no directed
  send to a single peer**, and `mPeers` holds peer *numbers*, not `ENetPeer*` handles, so adding one
  requires retaining the handle. This is what makes a per-peer late-join manifest (§3.5) an
  increment of its own.
- `PhysicsSystem::Update` adapts `realHZ`/`realDT` at runtime from measured frame cost
  (`:123-141`). **Two runs of the same scenario do not take the same number of physics substeps.**
  Reproducible measurement requires pinning this (§7.2).
- `TestObject.h:5` includes `../DistributedGameServer/ServerWorldManager.h` — a core class reaching
  up into a role directory. New code must not deepen this inversion.
- Several existing packets carry `std::string` members (`DistributedPhysicsClientConnectedToManagerPacket::ipAddress`,
  `StartDistributedGameServerPacket::createdServerIPs`, `GameStartStatePacket::levelSeed`) across a
  path that `memcpy`s the struct (`GameClient::SendPacket`, `:93-97`). These work only because SSO
  keeps the bytes inline and both ends are the same MSVC x64 binary. The `borderStr[256]` field
  carries an explicit comment saying exactly this (`NetworkObject.h:226-230`).
  **Every new packet in this design is strict POD with fixed-size arrays. No exceptions.**

---

## 1. Authority model

### 1.1 Options considered

| Option | Fit for this architecture |
|---|---|
| **A. Server-authoritative, blind input forwarding** — client sends to any server, that server forwards to the owner | Correct but wasteful. Every command costs an extra hop and an extra serialisation, and the client already holds the information needed to avoid it (§0.6). |
| **B. Client prediction + server reconciliation** | **Not viable here.** Reconciliation requires rolling the local sim back to an authoritative state and replaying. `PhysicsSystem` has no snapshot/restore. `NetworkObject::stateHistory` stores only position/orientation (`NetworkState`) — never velocity, force, or angular state — so a rollback cannot even be *expressed*. `ReadFullPacket`/`ReadDeltaPacket` hard-set the transform with no blending. Building rollback is a larger project than the entire interaction toolset, and it puts a second simulation in the client, muddying the dissertation's "the servers are the simulation" claim. |
| **C. Handoff-aware client routing + server-authoritative execution** | Zero new client state (§0.6), one hop in the common case, degrades gracefully to A on a stale route. |

### 1.2 Recommendation: **C — server-authoritative execution with client-side routing and owner-side relay on misroute**

Stated as an invariant:

> **The client's route is a hint. Ownership is always resolved server-side, from position, at the
> moment of application. Correctness never depends on the client's routing being right.**

That property is what makes the design defensible without prediction: a stale hint costs one relay
hop (≈ one RTT), never a wrong result. It also means the routing table can be arbitrarily stale
during a burst of migrations without any correctness consequence — only a latency one.

**Player-controlled objects get no special ownership rule.** A player avatar is an ordinary
networked object carrying a `controllerPlayerID`; whichever server currently owns the region
containing it applies that player's input. Introducing a separate "player home server" concept
would fork the handoff protocol into two cases — precisely the thing the constraint forbids.

### 1.3 Input in flight when the object migrates

Split commands into two classes, because they need **opposite** treatment:

**Continuous input (movement axes) is *state*, not an event.** The client re-sends it every client
tick, so a command dropped during migration self-heals within one send interval. It must **not** be
relayed — a relayed stale axis state is worse than no axis state, because it applies input the
player has already released. To remove the one-tick stall at the moment of handoff, the current
axis state travels **inside the handoff payload**. That is the only additive change to
`StartSimulatingObjectPacket` in this whole design, and it is deferred to increment 7.

**Discrete commands (spawn / destroy / impulse / teleport) are *events*.** They must never be
dropped and never applied twice. They are relayed on misroute with a hop limit of 1, and
deduplicated server-side.

Worst case for a discrete command issued exactly as its target migrates:

| t | Event |
|---|---|
| 0 | Client sends `Impulse(obj=42)` to server A (`mObjectOwner[42] == A`) |
| 0+ε | A dispatches obj 42's handoff to B. A **retains** `mNewServerID = B` until acked (increment 6) |
| 0+RTT | A receives the impulse, finds obj 42 inactive, sees `mNewServerID == B`, relays to B with `hopCount = 1`, acks the client `Relayed` |
| … | B applies it and acks the **client** directly with `Applied` |

If B has migrated the object onward again, B drops the relay and NACKs `NotOwner` with
`correctedServerID`; the client re-sends once with the corrected hint. Bounded at one retry — no
relay chains, no distributed consensus.

### 1.4 Classes that change

| Class | Change |
|---|---|
| `DistributedMultiplayerGameScene` | Routing table (already has `mServerRegions` + `mObjectOwner` + `mActiveServerId`), command sequence counter, ack handling, send path, client-side tombstones. |
| `DistributedGameServerManager` | Five new `ReceivePacket` cases that do nothing but look the command up in the registry and invoke it; relay dispatch; dedupe maps; ack emission. |
| `ServerWorldManager` | Implements `ICommandContext` — the only place that touches world/physics state. |
| `TestObject` | Gains `mControllerPlayerID` and an axis-state setter; `ReceiveClientInputs` retained unchanged. |
| `NetworkObject` | `CancelPendingTransition()`; `mNewServerID` retained until ack. **No wire-format change until increment 7.** |
| `PhysicsSystem` | `RegisterObject` / `UnregisterObject` (prerequisite, increment 1). |

---

## 2. Command channel

### 2.1 New message types

Appended to `BasicNetworkMessages` in `CSC8503CoreClasses/NetworkBase.h`, **after** the current
final entry `AddTestObjectsToTheWorld` (`:51`). Appending preserves every existing numeric ID;
inserting anywhere else silently renumbers the wire protocol for all four roles, which are built
and deployed separately (`tools/build-deploy.ps1`) and can therefore be mismatched at runtime.

Note the enum already carries ~15 dead team-game types (`ClientSyncItemSlotUsage` … `GuardSpotSound`,
`:24-37`). **Do not reclaim them.** Reusing a dead slot renumbers nothing but invites a stale
deployed binary to interpret a new packet as an old one. They cost nothing; leave them.

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

**Five types total, forever.** Adding a new *interaction* (grab, teleport, weld, …) adds **zero**
message types — that is the entire point of the registry in §6.

### 2.2 Packet layouts

Placed in `CSC8503CoreClasses/NetworkObject.h` alongside every other packet (inside the existing
`#ifdef USEGL` block, §0.12); constructors in `NetworkObject.cpp`. All strict POD, and all using
the **correct** `size = sizeof(X) - sizeof(GamePacket)` form (§0.5).

```cpp
	// Client -> owning game server. Reliable. One packet shape for every interaction
	// type; the payload is interpreted by the registered IInteractionCommand.
	struct DistributedClientCommandPacket : public GamePacket {
		int commandType;                        // NCL::Interaction::CommandType
		int sequence;                           // per-client monotonic; dedupe + ack key
		int hintServerID;                       // client's belief about the owner, -1 = unknown
		NCL::Interaction::CommandArgs args;     // POD, fixed size

		DistributedClientCommandPacket(int commandType, int sequence, int hintServerID,
			const NCL::Interaction::CommandArgs& args);
	};

	// Game server -> the issuing client. Reliable. Sent by whichever server APPLIED
	// the command, which is not necessarily the one that received it.
	struct DistributedCommandAckPacket : public GamePacket {
		int sequence;
		int playerID;
		int result;                             // NCL::Interaction::CommandResult
		int correctedServerID;                  // on NotOwner: where it should have gone; else -1

		DistributedCommandAckPacket(int sequence, int playerID, int result, int correctedServerID);
	};

	// Game server -> game server, over the directed peer mesh (§0.8). Carries the
	// client's identity so the true owner can ack the client directly, and the
	// relaying server's identity for the dedupe key on area effects, which fan out
	// to several peers at once.
	struct DistributedServerCommandRelayPacket : public GamePacket {
		int commandType;
		int originServerID;
		int originSequence;                     // (originServerID, originSequence) = dedupe key
		int hopCount;                           // 0 on send; >0 on receive is a bug: drop + count
		int playerID;
		int clientSequence;                     // so the applying server can ack the client
		NCL::Interaction::CommandArgs args;

		DistributedServerCommandRelayPacket(int commandType, int originServerID, int originSequence,
			int playerID, int clientSequence, const NCL::Interaction::CommandArgs& args);
	};
```

Spawn/despawn packets are in §3.3 / §4.1.

Add next to each:

```cpp
	static_assert(std::is_trivially_copyable_v<DistributedClientCommandPacket>);
	static_assert(sizeof(DistributedClientCommandPacket) ==
		sizeof(GamePacket) + sizeof(int) * 3 + sizeof(NCL::Interaction::CommandArgs));
```

This turns a future `std::string` member — the §0.13 trap — into a **compile error** rather than a
wire corruption that only manifests across machines.

### 2.3 Sequence numbers and acking

**Sequencing.** The client keeps one monotonic `mNextCommandSequence`, incremented per discrete
command, shared across all N server links (a global sequence, not per-link — it must be unique per
client so `(playerID, sequence)` is a valid dedupe key regardless of which server applies it).
Continuous input reuses the existing `ClientPlayerInputPacket::lastId` path and is **not**
sequenced; it is idempotent state.

**Delivery.** Client→server commands go reliable (`GameClient::SendReliablePacket`), so ENet
guarantees ordered, exactly-once delivery **on that link**. Reliability does *not* compose across a
relay hop — the relay is a separate reliable send on a different link, with no ordering
relationship to the first. That is precisely why the ack still matters and why dedupe is required
despite reliable transport.

**Acking is for result, not delivery.** `DistributedCommandAckPacket` carries a `CommandResult` so
the client can (a) clear optimistic UI, (b) learn the corrected owner on `NotOwner`, and (c) feed
the I4 accounting invariant in §7. Sent by the server that *applied* the command.

**Deduplication.** Each server keeps, per player:
- `mLastAppliedSequence[playerID]` — high-water mark; anything `<=` it is a duplicate;
- a 64-entry ring of recently seen sequences, because a relayed command can arrive out of order
  relative to a directly-routed one.

And per relaying server, for area effects: `mSeenRelay[(originServerID, originSequence)]` with the
same ring. **Without this, an object inside two overlapping relayed explosion radii receives the
impulse twice** — visible as 2× velocity, and a silent corruption of any measurement.

### 2.4 Routing on a client connected to N servers

New method `DistributedMultiplayerGameScene::ResolveCommandTarget(const CommandArgs&, const CommandScope&)`:

1. **Object-targeted** (`scope.targetsObject`): look up `mObjectOwner[targetObjectID]` — already
   maintained from every snapshot (§0.6). Hit → that link.
2. **Point-targeted** (`scope.targetsPoint`): test `worldPoint` against `mServerRegions` using the
   **same half-open rule the servers use** (§3.4). Hit → that link.
3. **Neither resolves**: reject locally with `CommandResult::Rejected` and log. **Do not
   broadcast** — a broadcast command would be applied N times by N servers, which is the exact
   failure mode this section exists to prevent.

The resolved server ID is also written into `hintServerID`, so server-side telemetry can
distinguish "the client guessed and was wrong" from "the client had no idea".

### 2.5 Misroute behaviour — **forward (relay), with the correction piggybacked on the ack**

The three candidates:

| Option | Verdict |
|---|---|
| **Drop** | Loses events. For a discrete command that is unacceptable — a destroy that vanishes leaves an object alive forever, breaking invariant I2. Rejected. |
| **Reject-with-redirect only** | Costs a full extra RTT on every misroute, and can **livelock**: if the object is migrating faster than the client's RTT (entirely possible for a fast object near a border, which is exactly the interesting case for this paper), the client's corrected hint is stale on arrival too, and the command never lands. Rejected as the sole mechanism. |
| **Forward with hop limit 1** | Guarantees bounded delivery: the command lands on the first relay in the common case, and the pathological case degrades to one retry rather than an unbounded loop. **Recommended.** |

**Chosen: forward, and additionally carry `correctedServerID` in the ack** so the client's routing
table self-heals as a side effect. This is strictly better than either pure option: forwarding
gives delivery, the correction gives convergence, and neither costs an extra round trip.

Server-side algorithm, in `IInteractionCommand::Apply` via the context:

```
effectivePoint = object position (if object-targeted) else args.worldPoint
resolvedOwner  = ctx.GetOwningServer(effectivePoint)

if (resolvedOwner == myServerID)   -> apply; ack Applied
if (resolvedOwner == -1)           -> ack Rejected            (outside the world)
if (relay.hopCount > 0)            -> ack NotOwner with correctedServerID; DO NOT relay again
otherwise                          -> relay to resolvedOwner (hopCount = 1); ack Relayed
```

For an object-targeted command whose object is not active here, `effectivePoint` comes from the
object's last known transform in `mCreatedObjectPool` — **the pool entry survives handoff**
(§0.1), so even a released object still yields a usable position. If the object is in this server's
`mNewServerID` record (mid-handoff, increment 6), that record is preferred over the positional
guess, because it is authoritative rather than inferred.

**Loop safety is structural**, not heuristic: `hopCount` is 0 or 1, and a relay is never
re-relayed. There is no scenario in which a command circulates. A `hopCount > 0` on receive is a
hard protocol violation and is counted (R2, §7.1).

### 2.6 Registration points

Following the asymmetric pattern of §0.8 exactly:

| Location | Add |
|---|---|
| `DistributedGameServerManager::RegisterPacketSenderServerPackets()` (`:143-151`) | `DistributedClientCommand` (from clients), `DistributedServerCommandRelay` (directed from peers) |
| `DistributedGameServerManager::ConnectServerToAnotherGameServer()` (`:482-496`) | `DistributedObjectSpawned`, `DistributedObjectDespawned` on the **outbound peer `GameClient`**, alongside the existing `StartSimulatingObjectInServer` registration (`:491`) — these are broadcasts, so they arrive the same way the handoff does |
| `DistributedMultiplayerGameScene::ConnectClientToDistributedGameServer()` (`:58-74`) | `DistributedCommandAck`, `DistributedObjectSpawned`, `DistributedObjectDespawned` |

---

## 3. Runtime spawn

### 3.1 The `mDynamicObjectList` blocker (§0.2) — concrete fix

**Rejected: rebuilding the broadphase every frame.** It is O(n) quadtree insertion per tick, and —
decisively — it would change measured physics time, contaminating the baseline the dissertation
compares against. The fix must be free for the existing path.

**Recommended: explicit registration, with a seeded flag replacing the `mStaticTree.Empty()`
sentinel.**

```cpp
// PhysicsSystem.h
public:
	// Incremental broadphase membership. Must be called for any object added to or
	// removed from the GameWorld after the first physics tick; the initial world is
	// still bulk-seeded on the first BroadPhase call exactly as before.
	void RegisterObject(GameObject* o);
	void UnregisterObject(GameObject* o);

protected:
	bool mBroadphaseSeeded = false;
	std::vector<GameObject*> mPendingUnregister;
	void FlushPendingUnregisters();
```

`BroadPhase` changes by one line — `if (mStaticTree.Empty())` becomes
`if (!mBroadphaseSeeded) { ... mBroadphaseSeeded = true; }`. **Behaviour for the existing path is
byte-identical**, because the world is fully seeded before the first tick (floor in the
`ServerWorldManager` constructor, objects in `CreatePlayerObjects` during
`HandleStartGameServerPacketReceived`, both strictly before `mIsGameStarted`).

Three details that are easy to get wrong and are the actual substance of this increment:

1. **Unregistration must be deferred.** `IntegrateAccel` and `IntegrateVelocity` iterate
   `mDynamicObjectList` **by index** (`:536`, `:577`), and `BroadPhase` does a nested index loop
   over it (`:460-501`). Erasing mid-iteration is undefined. `UnregisterObject` therefore pushes
   onto `mPendingUnregister`; `FlushPendingUnregisters` runs at the **top of `PhysicsSystem::Update`**,
   outside every iteration.
2. **Unregistration must purge the collision sets.** `mAllCollisions` holds raw `GameObject*` in
   `CollisionInfo::a/b` and `UpdateCollisionList` dereferences them to call `OnCollisionEnd` for up
   to `mNumCollisionFrames` frames afterwards (§0.2). `FlushPendingUnregisters` must
   `erase_if` both `mAllCollisions` and `mBroadphaseCollisions` of any entry referencing the object,
   **without** firing `OnCollisionEnd` on it.
3. **`ClearForces` gets a null guard** (`:610-616`), matching the guard `IntegrateAccel` already
   has. A spawned object mid-construction, or any object without a `PhysicsObject`, currently
   crashes the server here.

`ServerWorldManager` calls `mPhysics->RegisterObject(obj)` immediately after
`mGameWorld->AddGameObject(obj)` for runtime spawns, and `UnregisterObject` before
`RemoveGameObject` for destroys. `GameWorld::AddGameObject` is not virtual and `PhysicsSystem` does
not observe the world, so this must be wired explicitly at the call sites — there is no hook to
hang it on.

> **This increment also repairs the predictive path.** `PredictFuturePositions` iterates
> `mDynamicObjectList` (§0.2), so without this fix a spawned object would never get a predicted
> position and would never be handed off. That makes increment 1 a hard prerequisite for spawn,
> not merely a convenience.

### 3.2 Collision-free global ID allocation

| Option | Assessment |
|---|---|
| **Manager-issued lease** — servers request ID blocks from `SystemManager` | `SystemManager` is not in the object data path at all today; it orchestrates bootstrap and then goes quiet. Putting ID allocation there adds a round trip to every spawn (or block-exhaustion stalls if batched), makes the manager an availability single point of failure for *gameplay* rather than just startup, and **directly weakens the dissertation's "no central physics bottleneck" claim**. A reviewer would be right to press on it. |
| **Static high-bit partitioning** | One bit-shift, zero round trips, provably collision-free, no new failure mode. **Recommended.** |

The lease's only real advantage — a dense ID space — buys nothing here: nothing in the system
indexes objects by contiguous ID (`mCreatedObjectPool` is a `std::map`, `mNetworkObjects` a vector
scanned linearly).

New shared header `CSC8503CoreClasses/DistributedSystemCommonFiles/NetworkIdSpace.h`
(header-only, **not** `USEGL`-guarded, so the Tier-0 tests can include it — §0.12):

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
	// The pre-seeded range is left EXACTLY as it is today (dense ints from
	// ServerWorldManager's NETWORK_ID_BUFFER = 10), so pre-seeding, handoff, and the
	// dissertation's baseline measurements are untouched by this scheme.
	constexpr int kRuntimeFlag         = 1 << 30;
	constexpr int kServerShift         = 22;
	constexpr int kServerMask          = 0xFF;
	constexpr int kCounterMask         = (1 << 22) - 1;
	constexpr int kMaxRuntimePerServer = kCounterMask;

	inline bool IsRuntimeId(int id)    { return (id & kRuntimeFlag) != 0; }
	inline int  OriginServerOf(int id) { return IsRuntimeId(id) ? ((id >> kServerShift) & kServerMask) : -1; }
	inline int  MakeRuntimeId(int serverID, int counter) {
		return kRuntimeFlag | ((serverID & kServerMask) << kServerShift) | (counter & kCounterMask);
	}
}
```

`ServerWorldManager` gains `int mRuntimeIdCounter = 0;` and allocates with
`NetworkIdSpace::MakeRuntimeId(mServerID, mRuntimeIdCounter++)`, refusing (logging + counting) past
`kMaxRuntimePerServer`.

> **`OriginServerOf` is for diagnostics only.** Current ownership is *always* positional. Deriving
> live ownership from the ID would be wrong the instant the object crosses a border, and would
> quietly reintroduce the "player home server" mistake §1.2 rejects. The only legitimate use is the
> R5 assertion in §7.1.

### 3.3 Which server owns a spawn, and how peers learn of it

**Owner:** the server whose region contains the spawn point, `ctx.GetOwningServer(args.worldPoint)`.
If that is not the receiving server, the spawn relays like any other command (§2.5). **Spawn is not
a special case in the routing layer.**

**Visibility** is where §0.1/§0.3 bite. `StartHandlingObject` requires the target server to
*already have a pool entry* for the ID, so **every server must learn about a runtime spawn, not
just the owner.** Two mechanisms, and the design uses **both**:

- **(a) Mechanism — broadcast `DistributedObjectSpawned` reliably.** The existing
  `mDistributedPacketSenderServer->SendGlobalReliablePacket(...)` reaches peers *and* clients in one
  call, exactly as `StartSimulatingObjectPacket` already does (§0.8). Peers build a **deactivated**
  pool entry — reproducing the pre-seed model precisely; the owner builds an active one; clients
  build a replica. **Handoff then works with zero changes.**
- **(b) Safety net — lazy construction on handoff miss.** `StartHandlingObject` replaces
  `mCreatedObjectPool.at(id)` with a checked `find()`; on a miss it constructs the object from the
  archetype ID carried in the handoff packet rather than throwing (§0.3).

(a) alone is fragile to a lost broadcast. (b) alone silently changes the *meaning* of the handoff
protocol — the paper's core contribution — from "reactivate the pre-seeded twin" to "materialise on
demand". **Using (a) as the mechanism and (b) strictly as a counted recovery path keeps the
protocol's semantics intact while removing the crash.**

This is also why the spawn packet carries an **archetype ID, not a description**: every server must
build a byte-identical object, and passing a description over a `memcpy`'d struct is exactly the
`std::string`-on-the-wire trap of §0.13.

```cpp
	// Owning game server -> peers (create a DEACTIVATED pool entry, mirroring the
	// pre-seed model) and clients (create a replica). Reliable, broadcast on the
	// packet-sender server.
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

A small `ObjectArchetype` enum + table lives beside `ServerWorldManager::AddCubeToWorld` /
`AddSphereToWorld` (`:342-391`), which are already the de-facto prefab factory — they just need an
enum in front of them and the `rand() % 2` shape choice (§0.1) removed in favour of an explicit
archetype.

### 3.4 Spawn point exactly on a border

Fix the ambiguity at its source (§0.11) rather than special-casing spawn.

**Make `GetObjectServer` the single ownership authority, and define every region as half-open on
both axes** — `[minX, maxX) × [minZ, maxZ)` — with only the world's far edges closed. The regions
already extend to the world maximum in `GameInstance::CalculateServerBorders`
(`DistributedPhysicsServerDto.cpp:89+`), so closing the outer edge is a one-line clamp against the
world max.

`IsObjectInBorder` then becomes a one-liner that **cannot** disagree with `GetObjectServer`:

```cpp
bool ServerWorldManager::IsObjectInBorder(const Maths::Vector3& p) const {
	return GetObjectServer(p) == mServerID;
}
```

Every point in the world now maps to **exactly one** server — for spawn, handoff, pre-seeding, and
destroy alike. A spawn on a border stops being a special case; it is simply owned by whichever
server the half-open rule assigns it to. The client uses the identical rule in
`ResolveCommandTarget` (§2.4), so client and server never disagree about routing either.

After ownership is decided, nudge the object a hair inside its owner's region so it is not
immediately re-flagged for handoff on its first tick. That is exactly what
`CalculateIncomingObjectOffsetPosition` was written to do — **repair it** (fix the dangling
`const Vector3&` return, §0.11) and reuse it rather than adding a parallel mechanism.

> ⚠️ This is one of only two changes in the whole design that alter existing semantics. It changes
> which server claims an exact-border position. It is isolated to increment 2 and **gated on the I5
> handoff-parity counters landing first** (increment 0), so any regression is attributable.

### 3.5 How clients learn of new objects, including late join

**Steady state:** the `DistributedObjectSpawned` broadcast (§3.3) reaches clients directly; the
client creates a replica keyed by object ID, idempotently.

**Late join.** There is no initial sync today. A late client just starts receiving snapshots, and
`HandleFullPacket` calls `SpawnReplica` on first sight of an unknown ID
(`DistributedMultiplayerGameScene.cpp:154-163`). That accidentally works for transforms but loses
the archetype, and can never convey an object that is momentarily not being broadcast — e.g. one
sitting in the handoff ownership gap (§0.7) at the moment the client joins.

**Target design (increment 8):** on `DistributedPacketSenderServer::AddPeer`, the server emits a
**manifest** — one `DistributedObjectSpawned` per object it currently owns, followed by a forced
full snapshot. Clients apply manifest entries idempotently by object ID.

**Interim (increment 5):** broadcast the manifest globally rather than to the joining peer.
Wasteful (every existing client re-receives it) but needs no new plumbing, and existing clients
already dedupe by object ID.

The directed version needs `GameServer::SendPacketToPeer(int peerNumber, GamePacket&)`, which
**does not exist** — `GameServer` has only `SendGlobalPacket`/`SendGlobalReliablePacket`, and
`mPeers` is an `int[20]` of peer *numbers*, not `ENetPeer*` handles (§0.13). A directed send
requires retaining the handle. Worth doing — and it is the natural moment to also fix the
20-peer heap overflow (§0.13) — but not worth blocking spawn on.

---

## 4. Runtime destroy

### 4.1 Explicit despawn packet — the argument is structural, not stylistic

Absence from a server's snapshot stream **already means** "this server no longer owns the object"
(§0.10) — it is exactly the state handoff produces. The client merges streams from all N servers,
so absence from server A's stream is the **normal, expected condition during every single border
crossing**. Overloading it with "destroyed" would delete an object from the client on every
migration. That alone settles it.

Two further reasons, both of which matter given the transport:

- Snapshots are **unreliable UDP broadcast** (`SendGlobalPacket`, `:266`). An inference rule would
  need a timeout, introducing a visible latency floor *and* false-positive deletions under loss.
- Deltas are only meaningful relative to a known full state, so "absence" is not even
  well-defined between full snapshots.

**Therefore: an explicit, reliable despawn packet.** The reliability is the whole point — this is
the one message that must not be lost, because unlike a snapshot it is not re-sent next tick.

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

Snapshots are unreliable and can be **reordered around** the reliable despawn — ENet gives no
ordering guarantee between a reliable and an unreliable packet. Without protection, a late
`Full_State` for a destroyed object re-creates it through `SpawnReplica`.

`DistributedMultiplayerGameScene` gains `std::unordered_set<int> mDespawnedObjects;`
`HandleFullPacket` checks it **before** calling `SpawnReplica`, and `HandleDeltaPacket` before
applying. Counting rejected resurrections gives invariant I3 (§7).

Because IDs are never recycled (§4.4), tombstones can be kept for the whole run. If memory ever
matters, retire them after a fixed wall-clock window comfortably longer than any plausible reorder.

### 4.3 Destroy racing an in-flight handoff

The ownership gap (§0.7) makes this a genuine race with four reachable windows:

**W1 — destroy arrives after the transition flag is set, before dispatch.**
The tick order (§0.9) guarantees the network pump runs before `HandleObjectTransitions`, so this is
the **common case** and it is cleanly winnable. **Rule: destroy wins.** Clear the flag, then
destroy:

```cpp
	void NetworkObject::CancelPendingTransition();   // mIsActualPosOutServer = false;
	                                                 // mIsWaitingHandshake  = false;
	                                                 // mNewServerID         = -1;
```

A pure local state reset. **No wire change, no protocol change.**

**W2 — destroy arrives after the object was already sent and released.**
The sender must relay the destroy to the new owner, but `HandleTransitionComplete` has already
wiped `mNewServerID` (§0.7). Change: **retain `mNewServerID` until the ack arrives**, and reset
only then. This is the one place where the *already scaffolded* ack path becomes load-bearing —
`SendTransactionHandshakePacket` (`:469-480`) and `HandleTransitionHandshakePacketReceived`
(`:312-321`, body commented out) both already exist. **Completing them is finishing the design
that is already in the code, not replacing it.**

**W3 — destroy arrives at the new owner *before* the object does.**
B has no object with that ID and must not drop the destroy. B records a **pending destroy** keyed
by object ID; when `StartHandlingObject` later runs for that ID it destroys instead of activating,
and emits the despawn. A server-side tombstone mirroring the client's.

**W4 — two destroys for the same object (one direct, one relayed).**
Destroy is idempotent. The second observes the tombstone and acks `AlreadyDestroyed`. This is one
of the reasons the dedupe rings of §2.3 exist.

### 4.4 ID recycling policy — **never reuse**

Monotonic counter, no free list.

Reuse would require distributed agreement on when every server *and* every client has retired an
ID — a distributed garbage-collection problem, which is a research topic in its own right and
contributes nothing to this dissertation's thesis. The partitioned space (§3.2) gives 4.19M runtime
spawns **per server**; a dissertation-scale evaluation cannot approach that. Guard exhaustion
explicitly: log, count, and refuse the spawn with `CommandResult::Rejected`.

Not reusing is also what makes tombstones permanently safe, which is what makes §4.2 simple.

### 4.5 Local teardown

**Do not `delete` the `GameObject` on the tick it is destroyed.** `mDynamicObjectList`,
`mStaticTree`, `mBroadphaseCollisions` and `mAllCollisions` all hold raw `GameObject*` (§0.2), and
`UpdateCollisionList` will dereference a freed pointer for up to `mNumCollisionFrames` frames.

Sequence:

```
SetActive(false)
mPhysics->UnregisterObject(obj)          // increment 1; defers structural removal + purges collision sets
mGameWorld->RemoveGameObject(obj, false) // note: andDelete defaults to false — keep it that way
mPendingDeletion.push_back(obj)
   ... delete at the TOP of the next tick, after FlushPendingUnregisters has run ...
```

Keep the pool entry as a tombstone rather than erasing it, so a late relayed command targeting the
object resolves to `ObjectDestroyed` rather than `ObjectUnknown` — a materially more useful ack for
both the client and the I4 accounting.

---

## 5. Forces and impulses across borders

### 5.1 Recommendation: **server-to-server event relay**, not a ghost band

An area effect is a command with an origin, a radius, and a magnitude. The owning server:

1. Applies it to its own active objects inside the radius.
2. Tests the sphere's AABB against every entry in `mServerBorderMap` — **already present on every
   game server** (`DistributedGameServerManager.h`, populated at `:402-412`), and N ≤ 20, so the
   cost is trivial.
3. For each overlapped foreign region, sends `DistributedServerCommandRelay` over the existing
   directed peer mesh (§0.8) with `hopCount = 1`.

Each receiving server applies the effect **only to objects it owns**. No server ever writes to an
object it does not own. **Ownership stays exactly where the handoff protocol put it** — which is
what makes this purely additive.

### 5.2 Why not ghost regions

A read-only ghost band — replicating a strip of foreign objects into each server's physics world —
would require: duplicated simulation of the band, an authority-arbitration rule for objects inside
it, and doubling the handoff protocol with ghost-enter/ghost-exit events. That is a second
dissertation, and it destabilises the exact protocol this paper is about. It is the right answer
for a system that needs cross-border *contact*; it is the wrong answer for one that needs
cross-border *fields* and must not perturb its core contribution.

### 5.3 Use impulses, not forces — and the honest accuracy limits

Relayed effects must use `PhysicsObject::ApplyLinearImpulse`, **not** `AddForce`. `AddForce`
accumulates into `mForce`, which `ClearForces` zeroes every tick (`:116`), so a force relayed with
a one-tick skew lands in a *different integration window* on each server and produces a different
result. An impulse is a direct velocity change: order-independent and skew-insensitive. It is *not*
duplicate-safe, which is precisely what the `(originServerID, originSequence)` dedupe of §2.3 is
for.

**State the limits plainly in the write-up rather than glossing them:**

1. **One-tick skew.** Relayed effects are applied inside the network pump, before
   `ServerWorldManager::Update` — so the effect integrates on the same tick locally and on the
   *next* tick remotely (≈16 ms at the current cadence). This is a real cost and should be
   **measured and reported**. Why it is acceptable: the relayed quantity is an impulse to
   *velocity*, not a correction to *position*, so a one-tick skew produces a bounded, one-off
   position error of `|Δv| · dt`. It does not compound and it does not diverge — unlike a position
   correction, which would fight the simulation every frame.
2. **Fields propagate; contact does not.** The relay propagates *field* effects — explosions, area
   pushes, gravity wells — exactly. It does **not** propagate *contact*: a crate shoved into
   another crate across a border will not transmit the collision, because `BroadPhase` skips
   `!HasPhysics()` and `SetActive(false)` clears `mHasPhysics`, so objects on opposite sides of a
   boundary pass through each other. **This is a pre-existing limitation of the system, not one
   this design introduces**, and it should be presented as such — it is a property of region-split
   physics without ghosting, and naming it honestly is stronger than pretending the relay closes it.
3. **No occlusion.** A relayed radial impulse ignores geometry, so an object behind a wall on the
   far side of a border is affected as if in the open. Consistent with how the *local* radial
   impulse behaves, so at least it is not a cross-border-specific artefact.

---

## 6. Toolset API surface

Goal: **adding a new interaction type touches no network dispatch code.** `ReceivePacket` gains its
five cases once and never grows again.

### 6.1 Placement and the `using namespace` quirk

New files go in `CSC8503CoreClasses/DistributedSystemCommonFiles/` — the one directory every role
already compiles, free of renderer/FMOD dependencies and (like the rest of that folder) **not**
wrapped in `#ifndef DISTRIBUTEDSYSTEMACTIVE`. Per §0.12 they are also **not** `USEGL`-guarded, so
the Tier-0 tests can include them without the engine.

Use a new sub-namespace `NCL::Interaction` rather than piling into `NCL::CSC8503`. **This matters
because of the documented quirk:** `CSC8503CoreClasses/NetworkObject.h:12` carries a **global**
`using namespace NCL::CSC8503;` (outside any namespace, deliberately, per the comment at `:9-11`).
Once that header is in a translation unit, every `CSC8503` name is visible unqualified — so any new
type sharing a name with a `CSC8503` type becomes **ambiguous at every use site**, in files neither
you nor the compiler will point at helpfully.

> **Concretely: do not name anything in `NCL::Interaction` `GameObject`, `Transform`,
> `NetworkState`, `NetworkObject`, `GameWorld`, `PhysicsObject`, `TestObject`, or `PlayerInputs`.**
> The names below are chosen to avoid all of them.

### 6.2 `InteractionCommand.h`

```cpp
#pragma once
#include <map>
#include <memory>
#include <vector>

#include "Vector3.h"

namespace NCL::CSC8503 { class GameObject; }

namespace NCL::Interaction {

	// Wire values. APPEND ONLY - these travel in DistributedClientCommandPacket::commandType.
	enum class CommandType : int {
		None      = 0,
		MoveAxis  = 1,   // continuous, state-like: not sequenced, never relayed
		Impulse   = 2,
		Spawn     = 3,
		Destroy   = 4,
		Grab      = 5,
		Teleport  = 6
	};

	enum class CommandResult : int {
		Applied = 0,
		Relayed,             // forwarded to the true owner; that server acks separately
		NotOwner,            // owner changed again; correctedServerID is populated
		ObjectUnknown,
		ObjectDestroyed,
		Duplicate,
		Rejected
	};

	enum class DespawnReason : int { Destroyed = 0, LeftWorld = 1 };

	// POD payload shared by every command type. Fixed size; no std::string, no
	// std::vector, no pointers: the ENet path memcpys these structs verbatim
	// (GameClient::SendPacket), which is why DistributedClientConnectToPhysicsServerPacket
	// uses a char borderStr[256] rather than a std::string.
	struct CommandArgs {
		int            targetObjectID = -1;   // -1 = none
		int            playerID       = -1;
		int            archetypeID    = 0;    // Spawn: which prefab
		int            flags          = 0;
		Maths::Vector3 worldPoint;            // spawn point / effect origin / teleport destination
		Maths::Vector3 direction;             // impulse direction or movement axis
		float          magnitude      = 0.0f;
		float          radius         = 0.0f; // > 0 => area effect, may cross region borders
	};

	// Where a command must execute. Derived from the args by the command ITSELF, so
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

		// Cheap, side-effect free. Run on the client before routing AND on the server
		// before Apply - the client-side call is an optimisation only, never a
		// substitute for the server-side one.
		virtual bool Validate(const CommandArgs& args) const { return true; }

		// Executed on the authoritative server only, inside the network pump, before
		// ServerWorldManager::Update for that tick (see tick order, section 0.9).
		virtual CommandResult Apply(ICommandContext& ctx, const CommandArgs& args) = 0;
	};

	// Everything a command is allowed to do. Implemented by ServerWorldManager, so
	// commands never see the network layer and the network layer never sees the world.
	class ICommandContext {
	public:
		virtual ~ICommandContext() = default;

		virtual int GetServerID() const = 0;

		// The SINGLE source of truth for ownership (section 3.4). -1 outside the world.
		virtual int GetOwningServer(const Maths::Vector3& worldPoint) const = 0;

		// Active on THIS server only; null if unknown, destroyed, or handed off.
		virtual CSC8503::GameObject* FindActiveObject(int networkObjectID) const = 0;

		// Last known transform for any object in the pool, active or not. Lets a
		// non-owner resolve where an object-targeted command should be relayed.
		virtual bool TryGetLastKnownPosition(int networkObjectID, Maths::Vector3& out) const = 0;

		// Returns the allocated networkID, or -1 on failure (ID space exhausted).
		virtual int  SpawnObject(int archetypeID, const Maths::Vector3& at, int spawnerPlayerID) = 0;
		virtual bool DestroyObject(int networkObjectID, DespawnReason reason, int destroyerPlayerID) = 0;

		virtual void ApplyImpulse(int networkObjectID, const Maths::Vector3& impulse) = 0;
		virtual void ApplyRadialImpulse(const Maths::Vector3& origin, float radius, float magnitude) = 0;
		virtual void SetMoveAxis(int networkObjectID, int playerID, const Maths::Vector3& axis) = 0;

		// Queued, not sent: the manager drains the queue after Apply returns, so a
		// command never blocks inside the network layer or re-enters it.
		virtual void RelayToServer(int serverID, CommandType type, const CommandArgs& args) = 0;

		// Every region the sphere overlaps, EXCLUDING this server. Backed by the
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

`DistributedGameServerManager::ReceivePacket` gains exactly this, once, and **never grows**:

```cpp
	case BasicNetworkMessages::DistributedClientCommand: {
		auto* packet = static_cast<DistributedClientCommandPacket*>(payload);
		HandleClientCommandPacket(packet, source);
		break;
	}
```

with `HandleClientCommandPacket` performing dedupe → ownership resolution → `registry.Find(type)->Apply(...)`
→ relay drain → ack. **Type-agnostic throughout.**

### 6.4 File-level change list

**New files**

| File | Contents |
|---|---|
| `CSC8503CoreClasses/DistributedSystemCommonFiles/NetworkIdSpace.h` | ID partitioning helpers (header-only, no guards). |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h` | The header sketched in §6.2. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.cpp` | `CommandRegistry` implementation. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommands.cpp` | The built-in command classes. |
| `DistributedGameServer/ServerCommandContext.h` / `.cpp` | `ICommandContext` impl bridging `ServerWorldManager` + `DistributedGameServerManager`. |

> Register the first four in the `distributed_system_common_files` set of
> `CSC8503CoreClasses/CMakePC.cmake` (**lines 200-211**, verified) and the last in
> `DistributedGameServer/CMakePC.cmake`. **Adding files to disk is not enough** — per CLAUDE.md the
> `CMake*.cmake` lists are the build's source of truth, and a missing entry fails at link time in
> only some roles.

**Modified files**

| File | Change |
|---|---|
| `CSC8503CoreClasses/NetworkBase.h` | Five enum entries appended after `AddTestObjectsToTheWorld` (`:51`). |
| `CSC8503CoreClasses/NetworkObject.h` / `.cpp` | Five packet structs + constructors (correct `size` form); `CancelPendingTransition()`; retain `mNewServerID` until ack; `static_assert`s on layout. |
| `CSC8503CoreClasses/PhysicsSystem.h` / `.cpp` | `RegisterObject`/`UnregisterObject`; `mBroadphaseSeeded` replaces the `mStaticTree.Empty()` sentinel; deferred unregister + collision-set purge; null guard in `ClearForces`; `--fixed-step` support pinning `realHZ`/`realDT`. |
| `CSC8503CoreClasses/GameServer.h` / `.cpp` | `SendPacketToPeer` + retained `ENetPeer*`; fix the `int[20]` overflow (increment 8). |
| `CSC8503CoreClasses/GameClient.h` / `.cpp` | `WriteAndSendCommandPacket`; keep `WriteAndSendClientInputPacket` and finally call it. |
| `CSC8503CoreClasses/TestObject.h` / `.cpp` | `mControllerPlayerID`; `SetMoveAxis`; keep `ReceiveClientInputs`. Do **not** deepen the `../DistributedGameServer/` include inversion (§0.13). |
| `CSC8503CoreClasses/Profiler.h` / `.cpp` | Counters: commands applied/relayed/rejected/duplicate, spawns, despawns, handoff parity, invariant violations. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/TelemetryReporter.cpp` | Emit the new counters in the `GameServer` and `Client` `@@STAT` rows. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/LaunchConfig.*` | No change needed — `Has`/`GetString`/`GetInt` already cover `--scenario`, `--seed`, `--fixed-step`, `--assert-invariants`. |
| `DistributedGameServer/ServerWorldManager.h` / `.cpp` | `ICommandContext` impl; spawn/destroy/impulse; runtime ID counter; pending-destroy tombstones; `mPendingDeletion`; **checked `find()` replacing both `at()` calls** (`:166`, `:213`); `IsObjectInBorder` delegates to `GetObjectServer`; repaired `CalculateIncomingObjectOffsetPosition`; archetype table; `rand() % 2` removed. |
| `DistributedGameServer/DistributedGameServerManager.h` / `.cpp` | Handler registrations (§2.6); five `ReceivePacket` cases; dedupe maps; relay queue drain; ack emission; `HandleObjectTransitions` honours a cancelled transition; per-player `mStateIDs` keying fix (`:183`). |
| `CSC8503/DistributedMultiplayerGameScene.h` / `.cpp` | Command sending + `ResolveCommandTarget`; sequence counter; ack handling; `mDespawnedObjects` tombstones + guards in `HandleFullPacket`/`HandleDeltaPacket`; spawn/despawn handlers; `RemoveReplica`. |
| `CSC8503/DistributedClientStart.cpp` | Input capture → command emission. **Lives inside the `#ifndef DISTRIBUTEDSYSTEMACTIVE` block** (mouse/keyboard/raycast are renderer-side); the headless path above it must still build and run with none of it — which is why the scenario driver (§7) sits *outside* that guard. |
| `docs/NETWORKING.md`, `docs/SPATIAL-PARTITIONING.md` | New packet catalogue rows; the positional-ownership rule; the half-open border convention. |

---

## 7. Determinism and test plan

The system produces paper measurements, so interactions must be **reproducible**. Today they would
not be, for three independent reasons — all three must be fixed or the numbers are not defensible.

### 7.1 Determinism prerequisites

| Source of nondeterminism | Fix |
|---|---|
| **Unseeded `rand()`** in `CreateObjectGrid` (`:237`, §0.1). No `srand` anywhere, so agreement is accidental and shape-per-ID can differ per server. | Add `int worldSeed` to `StartDistributedGameServerPacket` — it is the packet that already triggers `CreatePlayerObjects` (`:413-416`), so the seed arrives exactly where it is needed. Replace `rand() % 2` with an explicit `std::mt19937` seeded from it. (`GameStartStatePacket` already has a `levelSeed` field, but it is a `std::string` on a `memcpy` path — §0.13 — so do **not** use it.) The launcher passes `--seed`. |
| **Adaptive physics timestep.** `PhysicsSystem::Update` halves/doubles `realHZ`/`realDT` from measured frame cost (`:123-141`, §0.13), so two runs take different substep counts. | `--fixed-step` pins `realHZ`/`realDT` and feeds a constant `dt`. **Required** for any reproducible measurement, independent of this feature. |
| **Wall-clock command scheduling.** | The scenario driver replays commands at fixed **tick numbers**, never wall-clock times. |

Note these are *only* prerequisites for reproducibility, not for correctness — nothing else in the
design depends on them.

### 7.2 Races this design introduces, and how each is detected

| ID | Race | Detection |
|---|---|---|
| R1 | Command applied against a stale owner (client routing lag) | `NotOwner` ack counter. A small nonzero rate is normal; a spike means the routing table is not refreshing. |
| R2 | Relay loop A→B→A | `hopCount > 0` on receive is a hard violation; count and log. Structurally impossible (§2.5), so nonzero means a regression. |
| R3 | Command applied twice (direct + relayed, or two overlapping area relays) | Dedupe rings; count duplicate hits. A doubly-applied impulse shows as 2× velocity. |
| R4 | Destroy vs handoff, windows W1–W4 (§4.3) | Invariant I1, plus explicit counters for "destroy cancelled a pending transition" and "pending destroy consumed on arrival". |
| R5 | Two servers allocate the same runtime ID | Impossible by construction; still assert `OriginServerOf(id) == sender` on receipt and count violations. This catches a **mis-set `mServerID`**, which is the realistic failure. |
| R6 | Spawn broadcast lost → later handoff hits an absent pool entry | Today this **throws** (§0.3). Replace with `find()` + an `UnknownObjectOnHandoff` counter + the lazy-construct path (§3.3b). A nonzero count means mechanism (a) is failing and (b) is carrying the system. |
| R7 | Client resurrects a destroyed object from a reordered snapshot | Tombstone set; count rejected resurrections (I3). |
| R8 | Spawn exactly on a border → claimed by two servers or none | Invariant I1; eliminated by the half-open rule (§3.4), so nonzero means that rule regressed. |
| R9 | An object active on two servers at once | Invariant I1. **Latently possible today** given the eager send-and-release handoff (§0.7) — measuring it is valuable independent of this feature. |
| R10 | Spawned object never simulated / destroyed object dangling in `mDynamicObjectList` | Assert `mDynamicObjectList.size()` tracks the active count. A spawned object that never falls under gravity is the visible symptom. |

### 7.3 Minimum viable test approach — three tiers, cheapest first

There is no test suite today and validation is empirical (per CLAUDE.md). Tier 0 is where most of
the value is, and it is cheap enough that there is no excuse to skip it.

**Tier 0 — pure unit tests. No engine, no network, no window.**
A small `tools/InteractionTests` console executable, deliberately **outside** the CMake role toggle
so it can never perturb the four role builds. This is only possible because the toolset headers are
free of `USEGL`/`DISTRIBUTEDSYSTEMACTIVE` guards (§0.12/§6.1) — which is a large part of why they
are placed as they are. Covers:

- **`NetworkIdSpace` round-trip and disjointness**: for sampled `(serverID, counter)`,
  `OriginServerOf(MakeRuntimeId(s, c)) == s`; no runtime ID collides with any pre-seeded ID; no two
  servers' spaces intersect.
- **`GetOwningServer` totality** against a synthetic border map: interior points, exact border
  points, world corners, points outside. The assertion is **every point maps to exactly one
  server** — this is the direct regression test for §0.11 and R8.
- **Routing resolution**: given a region map + owner table, `ResolveCommandTarget` picks the
  expected link, including the stale-owner-table case.
- **Dedupe window** under out-of-order sequences.
- **Packet layout**: `static_assert`s that every new packet is trivially copyable and that
  `size + sizeof(GamePacket) == sizeof(T)`.

Fully deterministic, runs in milliseconds, catches R2/R5/R8 and the whole class of memcpy-layout
regressions before anything is deployed.

**Tier 1 — the deterministic headless scenario (the "minimum viable scenario test").**
Most infrastructure already exists: `--headless`, `HeadlessRunner`, `@@STAT` telemetry
(`TelemetryReporter`), the launcher's parser, and per-role exit codes (commit `e0d887d`). Three
additions:

1. **`--scenario <file>`** on the client (parsed by the existing `LaunchConfig`), replaying a
   command list at fixed **tick numbers**. Must sit **outside** the `#ifndef DISTRIBUTEDSYSTEMACTIVE`
   guard so it works in a headless client with no renderer.
2. **`--fixed-step`** and **`--seed`** on the game server (§7.1).
3. **`--assert-invariants`** on the game server: check the invariants each tick, emit them as
   `@@STAT` keys, and **exit non-zero on violation** so the launcher surfaces the failure rather
   than burying it in a log tab.

**Canonical scenario** — 2 servers, world split at x = 0:

```
tick   0  : spawn 20 objects at x = -10   (archetype = cube, deterministic)
tick  60  : impulse +X on all 20          -> all cross the border
tick  90  : destroy 5 of them WHILE they are flagged for transition   (exercises W1/W2)
tick 120  : radial impulse centred exactly on x = 0, radius spanning both regions
tick 300  : assert and exit
```

This single scenario exercises spawn, destroy, cross-border push, the border-exact ownership rule,
and the destroy-vs-handoff race — i.e. every mechanism this design adds.

**Invariants:**

| ID | Invariant |
|---|---|
| **I1** | **Exactly one owner.** For every live object ID, the number of servers reporting it active is exactly 1 (0 if destroyed). Each server emits a per-tick digest (`objs=<n> idxor=<xor of active IDs>`); a small offline script (or the launcher) verifies the per-server ID sets are disjoint and their union is the expected set. **No cross-server runtime coordination needed** — the digests are compared after the run. |
| **I2** | **Conservation of object count.** `Σ spawned − Σ destroyed == Σ active`, globally. This is the "no lost objects" assertion. |
| **I3** | **No resurrection.** Client-side count of snapshots accepted for a tombstoned object is 0. |
| **I4** | **Command accounting.** `Σ commands sent by clients == Σ (applied + rejected + duplicate-dropped)` across all servers. Catches silently swallowed commands, which are otherwise invisible. |
| **I5** | **Handoff parity.** `Σ StartSimulatingObjectPacket sent == Σ StartHandlingObject succeeded`. **Add this FIRST, before any interaction work** — it is a baseline regression guard on the untouched handoff protocol, and it is what makes it safe to change the border rule in increment 2. |

**Tier 2 — soak / chaos.** The same scenario with randomised (but seeded) spawn/destroy/impulse at
high rate for ten minutes, asserting I1–I5 throughout. This is what catches W3 and R6, which are
timing-dependent and will not appear in a scripted run.

**Baseline capture — do this before touching anything.** Record physics time, world time, both
snapshot times, and objects-on-borders for 2-server and 4-server runs at the current object counts.
The dissertation needs the *delta* attributable to the interaction system, and **it cannot be
reconstructed after the fact.**

---

## 8. Implementation increments

Each increment is independently demonstrable and independently revertible.

| # | Increment | Demonstrates | Handoff impact |
|---|---|---|---|
| **0** | **Baseline + safety net.** I5 handoff-parity counters; both `mCreatedObjectPool.at()` → checked `find()`; fix the dangling return in `CalculateIncomingObjectOffsetPosition`; null guard in `ClearForces`; per-player `mStateIDs` keying. Capture the baseline measurements. | Existing runs behave identically; new counters appear in telemetry. | **None — purely additive.** |
| **1** | **`PhysicsSystem` dynamic registration.** `RegisterObject`/`UnregisterObject`; `mBroadphaseSeeded`; deferred unregister + collision-set purge. | A server-local test adds a cube after startup and it falls under gravity **and gets a predicted position**. | **None — purely additive.** |
| **2** | **Ownership unification.** `GetObjectServer` becomes sole authority; half-open on both axes; `IsObjectInBorder` delegates. | Per-server pre-seed counts sum **exactly** to the total (they may not today, on exact-border rows). | ⚠️ **Semantics only** — changes which server claims an exact-border position. **Gate on increment 0's I5 counters.** |
| **3** | **Command channel + `MoveAxis`/`Impulse`.** Enum entries, packet structs, registry, `ICommandContext`, client routing + ack, server dispatch + relay-on-misroute. | Click to push a pre-seeded object — including one owned by a different server than the client last believed. | **Additive.** |
| **4** | **Cross-border area effects.** Radial impulse with region-overlap relay + relay dedupe. | An explosion on the border visibly pushes objects on **both** sides. | **Additive.** |
| **5** | **Runtime spawn.** ID partitioning, `Spawn` command, `DistributedObjectSpawned` to peers + clients, peer pool-entry creation, archetype table, seeded RNG, broadcast manifest. | Spawn an object, watch it cross a border and be handed off correctly. | **Additive.** The handoff *packet* is unchanged; what is new is that peers create pool entries at runtime rather than only at pre-seed. |
| **6** | **Runtime destroy.** Despawn packet, client + server tombstones, W1–W4 handling, `CancelPendingTransition`, and **finishing the ack path** so `mNewServerID` survives until acknowledged. | Destroy an object mid-handoff; I1/I2 hold. | ⚠️ **The one increment that changes handoff semantics** — the currently-inert ack becomes load-bearing. Requires a dedicated A/B run against increment 0's baseline. |
| **7** | **Player-controlled avatar.** `TestObject` gains a controller ID; continuous input as state; current axis state appended to `StartSimulatingObjectPacket`. | Drive an avatar across a border with no input stall. | ⚠️ **The only wire change to the handoff packet.** Fields appended at the end of the struct. |
| **8** | **Late-join manifest + directed peer send.** `GameServer::SendPacketToPeer` with retained `ENetPeer*`; per-peer manifest on `AddPeer`; fix the `int[20]` overflow. | A client joining a running instance sees the full world immediately, including runtime spawns. | **Additive.** |

Increments 0–1 are prerequisites with no user-visible feature; 2 is a bug fix; **3 is the first
increment that demos as "interaction"**. If time is short, 0–5 is a coherent, publishable subset:
it delivers commands, cross-border forces, and spawn with the handoff protocol **completely
untouched**.

### 8.1 Handoff-protocol impact, summarised

The constraint rightly flags this as the thing not to destabilise. Explicitly:

**Purely additive — no change to handoff behaviour (increments 0, 1, 3, 4, 5, 8):**
- **Wire format unchanged.** `StartSimulatingObjectPacket` and `StartSimulatingObjectReceivedPacket`
  keep their exact layouts through increment 6.
- **Send-and-release eagerness: never changed.** The design does **not** convert handoff into a
  two-phase commit. The ownership gap (§0.7) is documented and measured, not closed.
- **Broadcast delivery of the transfer packet: never changed.** Every new server↔server message
  reuses the same asymmetric pattern (§0.8) rather than introducing a second one.
- **Pool reactivation: never changed.** Runtime spawns are made to **fit** the pre-seed model
  (peers get deactivated pool entries) rather than the model being changed to accommodate them.
  This is the single most important design choice in §3.

**Two deliberate semantic changes, each isolated to its own increment and gated on the I5 baseline:**

| Increment | Change | Risk | Mitigation |
|---|---|---|---|
| **2** | Which server claims an exact-border point (half-open on both axes). | Objects sitting exactly on a border change owner once, at startup. Could shift per-server object counts and therefore the measured load balance. | Gate on I5 parity counters from increment 0; re-run the baseline after and report both. The change strictly *reduces* ambiguity — today the two border tests disagree (§0.11), so some positions are already handled inconsistently. |
| **6** | The transition ack becomes load-bearing (`mNewServerID` retained until acked). | If an ack is lost, `mNewServerID` now persists where it previously cleared — a leak of transition state, and a destroy could relay to a server that no longer owns the object. | Bound it with a timeout that clears `mNewServerID` and counts the event; A/B against increment 0's baseline. Best framed as **completing** the two-phase design already scaffolded in the code — `SendTransactionHandshakePacket`, `mIsWaitingHandshake`, `OnTransitionHandshakeReceived` and the commented-out cleanup all already exist and are unused — rather than as a redesign. That framing is also the honest one. |

**Increment 7** appends fields to `StartSimulatingObjectPacket`. Appending is safe *only* because
all roles are rebuilt and redeployed together (`tools/build-deploy.ps1`); there is no version
negotiation on this wire protocol, so a mixed deployment would misparse. Note this in `DEPLOY.md`.

---

## 9. Open questions

1. **Should `MoveAxis` be a registry command at all?** It is state, not an event, and the existing
   `ClientPlayerInputPacket` path already carries it end-to-end (§0.5). Folding it into the registry
   is conceptually tidy but creates **two wire representations for the same thing**. Alternative:
   leave continuous input on `ClientPlayerInputState` and let the registry own only discrete
   commands. **Leaning toward the alternative**; decide in increment 3.
2. **Client-side smoothing.** With interaction, snapshot pops become far more noticeable than they
   are for passively falling cubes. Interpolating between the last two full states is orthogonal to
   this design and cheap — but it is **not prediction** and must not be described as such in the
   write-up.
3. **Should the manager know about spawns at all?** Currently no, and keeping it out of the data
   path is the right default (§3.2). The launcher dashboard would benefit from a global object
   count, but that can be had by summing per-server `@@STAT` counters in the launcher — free, and it
   preserves the no-central-bottleneck property.
4. **Does the 20-peer overflow (§0.13) invalidate any already-captured measurements?** Worth
   checking what configurations have been run. Any run with servers + clients > 20 should be
   discarded and repeated after increment 8.

---

## 10. Implementation notes — increment 1 (shipped 2026-08-16)

Plan: `docs/superpowers/plans/2026-08-16-physics-dynamic-registration.md`.

### What shipped

- `tools/InteractionTests` — the repo's first test target (Tier 0). 10 tests, all passing.
  It links the same libraries as `EntryPointServer` and needs the roles' precompiled-header
  list, without which `PhysicsSystem.h` fails on `std::set` and `PhysicsObject.h` on `Matrix3`.
- `PhysicsSystem::RegisterObject` / `UnregisterObject`, `FlushPendingUnregisters`,
  `mBroadphaseSeeded`, `mPendingUnregister`.
- **No call sites.** `ServerWorldManager` does not call either method; there is nothing to spawn
  or destroy until increments 5–6. The methods are tested, not yet used.

### The seed sentinel was worse than §0.2 described

§0.2 says `BroadPhase` seeds once, guarded by `mStaticTree.Empty()`. In a world with **no static
geometry** the tree stays empty forever, so the seed re-runs — and `BroadPhase` runs inside the
**substep** loop, not once per tick, so `mDynamicObjectList` grew by roughly six entries per tick.
The failing test measured 1 → 5 → 11 → 17 integrated objects for a single cube. Never hit in
production because every server world gets a floor in `ServerWorldManager`'s constructor.

`Clear()` must also reset `mBroadphaseSeeded`, which §3.1 does not mention: it empties
`mDynamicObjectList`, so without the reset a cleared world can never be re-seeded.

### `QuadTree` has no removal operation

§3.1's `UnregisterObject` design assumes one exists. It does not. Static objects therefore cannot
be unregistered; the implementation warns and refuses rather than leaving a dangling pointer in
`mStaticTree`. Harmless for the planned increments — runtime spawn only produces dynamic objects.

### Reproducibility: the measurement harness was not reproducible

Verifying "the baseline is unchanged" failed on its own terms. Two runs of the **identical binary**
gave different handoff counts (40/2 vs 40/1) and different object splits (362/38 vs 361/39).

**Cause.** `--fixed-step` pinned only the *substep* rate inside `PhysicsSystem`. The headless loop
still fed `Update` a measured wall-clock `dt`, and the run was bounded by wall-clock seconds, so
tick counts varied with machine load (28,653–29,431 over nominally identical 60 s runs).
`CheckPositionOutOfServerBoundaries` runs once per **tick**, so border checks landed at different
simulated times.

**First fix — and why it made things worse.** Pinning the loop `dt` and bounding by tick count made
each server advance exactly 7200 ticks, but let each advance at its own wall-clock rate: server 1
finished 7200 ticks in 1.2 s while server 0 took 11.2 s. The fast server **exited while the slow
one was still handing objects to it**, and those objects were lost outright — conservation dropped
to 397/400 and then 389/400. This is the §0.7 ownership gap made visible: the sender deactivates on
send, so a handoff to a dead peer loses the object permanently.

**Shipped fix.** Reproducible mode pins `dt` *and* paces each tick to `fixedDt` of real time
(`sleep_until`), so every server stays on the same shared clock without an explicit barrier.
Bootstrap ticks are excluded from the budget via `HeadlessRunOptions::countTicksWhen` — without
that gate a reproducible run consumed its entire 7200-tick budget during the handshake, in ~25 ms,
and exited before the world was built.

**Result.** Two paced runs, 7200 ticks each:

| Run | s0 owns | s1 owns | total | conservation |
|---|---|---|---|---|
| paced-a | 359 | 41 | 400 | holds |
| paced-b | 359 | 41 | 400 | holds |

End state is identical. Handoff *event* counts still differ by ±1 (42 vs 41), because message
arrival relative to a tick boundary is still wall-clock dependent. **Bit-identical event counts
would require a global tick barrier between servers**, which this architecture does not have.
Claims of reproducibility must therefore be scoped to end state and conservation, not to event
counts.

### The realtime baseline's p50 was measuring idle loop iterations

| | mean | p50 | p95 | p99 |
|---|---|---|---|---|
| baseline realtime s0 | 0.3832 | 0.0170 | 1.5842 | 2.1763 |
| paced-a s0 | 1.3392 | 1.3230 | 1.5808 | 1.7401 |
| paced-b s0 | 1.3689 | 1.3419 | 1.6582 | 2.1301 |

In realtime mode the loop spins at ~1 kHz while physics only substeps every 1/120 s, so roughly
seven ticks in eight do **no** physics work — hence `p50 = 0.017 ms`. That number is the cost of an
empty loop iteration, not of physics. Paced mode performs exactly one substep per tick, giving a
tight unimodal distribution. The two agree where it matters: **p95 is ~1.58 ms in both**, because
the realtime p95 is precisely the ticks that did work. Report paced percentiles; the realtime p50
is an artefact.

`integrated == owned` holds on **100%** of paced ticks (vs 0.14–0.25% mismatch in realtime), for
the same reason — no partially-updated ticks to sample.

### Verdict

Increment 1 is free: paced tick costs bracket each other, conservation holds exactly, `hoFail = 0`.

---

## 11. Implementation notes — increment 3 (shipped 2026-08-16)

Plan: `docs/superpowers/plans/2026-08-16-interaction-command-channel.md`.

### What shipped

- `InteractionCommand.h/.cpp` — `CommandType`, `CommandResult`, `CommandArgs`, `CommandScope`,
  `IInteractionCommand`, `ICommandContext`, `CommandRegistry`. Guard-free, so the client (which
  does not define `DISTRIBUTEDSYSTEMACTIVE`) and the test target both include it.
- `SequenceWindow.h` — high-water mark plus a 64-entry ring, for dedupe under out-of-order arrival.
- `InteractionCommands.cpp` — `Impulse` (object-targeted, relays on misroute) and `MoveAxis`
  (continuous state, dropped rather than relayed).
- Three appended message types, three POD packets with `static_assert`s on trivial copyability and
  size.
- `ServerWorldManager` implements `ICommandContext`. `SpawnObject`/`DestroyObject` are honest
  stubs returning `-1`/`false` pending the spawn and destroy increments.
- `DistributedGameServerManager` dispatch, per-player and per-origin dedupe, ack, relay drain.
- Client routing (`ResolveCommandTarget`), sending, and `NotOwner` owner-table correction.
- `cmdApplied` / `cmdRelayed` / `cmdDup` / `cmdRejected` on servers and `cmdSent` on the client,
  which is what makes I4 checkable from the `@@STAT` stream.

### Corrections to the plan, found while building

- **`BasicNetworkMessages` and `GamePacket` are at global scope**, not in `NCL` / `NCL::CSC8503`.
  The plan's test code qualified them wrongly.
- **The directed peer send already existed.** §0.8 describes the transfer as a broadcast, and the
  plan hedged that a directed send might have to be invented. It does not:
  `mDistributedPhysicsClients` holds `GameServerConnection*` and `SendTransactionHandshakePacket`
  already does a directed lookup by server ID. The relay reuses it. It *did* need
  `DistributedServerCommandRelay` registering on the outbound peer link in
  `ConnectServerToAnotherGameServer`, which previously registered only
  `StartSimulatingObjectInServer`.
- **`RegisterDefaults()` must also run on the client**, not just the servers: `SendCommand` looks
  the command up to derive its scope and to validate before routing.
- **A command driver was required to verify anything.** A headless client has no input path, so
  the channel was wired but never exercised and `cmdApplied` stayed 0. Added `--impulse-test N`
  on the client (opt-in, off by default), fires one impulse every N ticks at a rotating object id.

### Verification

Reproducible run, 2 servers, 400 objects, 7200 ticks, seed 42, `--impulse-test 20`:

| | server 0 | server 1 |
|---|---|---|
| objects owned | 357 | 43 |
| integrated | 357 | 43 |
| handoffs sent / received | 44 / 1 | 1 / 44 |
| commands applied | 246 | 2 |
| relayed / duplicate / rejected | 0 / 0 / 0 | 0 / 0 / 0 |

- **I4 holds exactly.** At the aligned 2 Hz sample the client reports `cmdSent=248` and the
  servers `246 + 2 + 0 + 0 = 248`. The client's final `cmdSent=253` is higher only because it
  keeps sending after the servers took their last sample and exited.
- **I5 holds exactly.** 45 sent, 45 received, `hoFail=0`.
- **Conservation holds.** 357 + 43 = 400.
- **Handoff is untouched**, as designed: 44/1 against the increment 1 baseline's 42/1, inside the
  ±1-per-run event jitter that section 10 established is inherent without a global tick barrier.

### Closing the relay gap — and the bug it exposed

The first live run left `cmdRelayed = 0`: the client's owner table is refreshed from 10 Hz full
snapshots, so it was never stale at the instant a command was issued. The real staleness window —
between a handoff and the next snapshot — is a few milliseconds wide and cannot be hit reliably
from outside. Added `--misroute-every N` on the client, which sends every Nth driven command to a
server that demonstrably does **not** own the object, reproducing the condition on demand via a new
`SendCommandTo(..., forcedServerId)`.

That immediately exposed a real defect. With misrouting on, I4 came out as:

```
sent 1502  =  applied 992 + rejected 14 + dup 0   ->  gap 496
relayed                                            =       496
```

The gap equalled the relay count exactly: **every relay was being counted as sent and then
silently dropped.** The diagnostic added to `DrainPendingRelays` reported
`no peer link to server 0 for relay; have 1 link(s): 1` — server 1's only peer link was labelled
with its *own* id.

**Root cause.** `HandleStartGameServerPacketReceived` used the loop index `i` as the peer's server
id. But `StartDistributedGameServerPacket` has two differently-indexed families of arrays:
`serverIDs[]` and `borders[]` are indexed **by server id** and run to `totalServerCount`, while
`serverPorts[]` and `createdServerIPs[]` are filled **in registration order** and run to
`currentServerCount`. Whenever servers registered in an order other than their id order, every
peer link got the wrong label.

**This was never specific to commands.** `SendTransactionHandshakePacket` does the same
`connection->serverID == senderServerID` lookup, so the transition **ack could never find its link
either** — a concrete mechanism behind the "handoff ack is stubbed" audit finding.

**Fix.** Added `connectedServerIDs[20]` to the packet, aligned with the IP/port arrays and
populated by the manager from `GetServerID()`; the receiver uses it instead of the index, and
skips an entry with a `-1` id rather than falling back to the index. Also made the loop guard real:
the packet constructor always stamps `hopCount = 0`, so a relay emitted while handling a relay was
indistinguishable from a first hop. `mCurrentRelayHop` now stamps it correctly.

### Verification (final)

Reproducible run, 2 servers, 400 objects, 7200 ticks, seed 42, `--impulse-test 20
--misroute-every 3`. The client is bounded 15 s shorter than the servers so every command it sent
is processed before they exit, and both ends print exact `@@FINAL` totals rather than 2 Hz samples:

| | server 0 | server 1 |
|---|---|---|
| objects owned | 362 | 38 |
| handoffs sent / received | 39 / 1 | 1 / 39 |
| commands applied | 1382 | 104 |
| relayed / duplicate / rejected | 35 / 0 / 0 | 460 / 0 / 14 |

- **I4 exact:** `1486 applied + 14 rejected + 0 duplicate = 1500` = `cmdSent 1500`. **Gap 0.**
- **I5 exact:** 40 sent, 40 received, `hoFail = 0`.
- **Conservation exact:** 362 + 38 = 400.
- 495 relays, **0** dropped for a missing peer link, **0** routing loops.

The relay path is now covered end to end, not just by unit test.

---

## 12. Implementation notes — increment 2 (shipped 2026-08-16)

### What shipped

`DistributedSystemCommonFiles/RegionOwnership.h` — a guard-free header holding POD `RegionBounds`
and `OwningServerFor(regions, point)`. Regions are half-open on **both** axes,
`[minX, maxX) x [minZ, maxZ)`, with only the world's outer maximum closed (derived from the regions
themselves, so a caller cannot supply an extent that disagrees with the partition).

All three former copies of the rule now delegate to it:

- `ServerWorldManager::GetObjectServer` — the single ownership authority.
- `ServerWorldManager::IsObjectInBorder` — now `GetObjectServer(p) == mServerID`, so it *cannot*
  disagree.
- `DistributedMultiplayerGameScene::ResolveCommandTarget` — the client calls the **same function**
  rather than reimplementing the same rule, which is what stops client and server drifting.

`GetObjectServer` runs once per object per tick, so the region list is cached
(`mCachedRegions`, lazily rebuilt when the border map's size changes — the map is populated after
construction, when the manager's start packet arrives, so a one-shot copy in the constructor would
have been empty).

### Verification

- **7 new unit tests**, 36 total, all passing. `EveryPointMapsToExactlyOneServer` sweeps a grid that
  lands exactly on both interior seams and both outer edges of a 2x2 world and asserts every point
  is claimed by exactly one server — never zero, never two. That is the property the increment
  exists to establish.
- `SharedBorderBelongsToTheHigherRegion` pins the semantic change: the point `(0,0,0)` on a shared
  seam previously went to server **0** (closed on both axes, lowest id wins by `std::map` order)
  and now goes to server **1**.
- **2-server live run** (7200 ticks, seed 42, with command traffic): I4 gap **0**
  (1487 applied + 14 rejected = 1501 sent), I5 exact (38 = 38), `hoFail = 0`, conservation
  362 + 38 = 400.
- **4-server live run** (3600 ticks, seams on both axes): conservation exact (328 + 3 + 31 + 38 =
  400), I5 exact (88 = 88), `hoFail = 0`.

### Honest limitation of the live evidence

The spec's stated demo for this increment is "per-server pre-seed counts sum exactly to the total
(they may not today, on exact-border rows)". Read from the per-tick CSVs at tick 0, the 4-server
run gives `400 + 0 + 0 + 0` — exactly 400, **but** because the default world spawns every object
inside one region. No object lands on a seam at pre-seed in this workload, so the live check passes
*vacuously*: it confirms no regression, not that the old bug was triggered and fixed.

The real evidence is the unit tests. Demonstrating the old defect live would need a workload that
deliberately seeds objects on `x = 0` / `z = 0`; worth adding when the evaluation workloads are
built out, and cheap once `--workload` grows a second mode.

Note the old `IsObjectInBorder` was closed on Z (`z <= maxZ`), so on a 4-server grid an object at
exactly `z = 0` would have been accepted as in-border by **both** the region below and the region
above — activated twice, integrated twice, and broadcast by two servers. The half-open rule removes
that class of bug by construction rather than by testing for it.

---

## 13. Implementation notes — increments 4 and 5 (shipped 2026-08-16)

### Increment 4 — cross-border area effects

`Impulse` with `radius > 0` becomes a point-targeted area effect (`GetScope` returns
`targetsPoint + isAreaEffect`), so it routes by point and needs neither a target object nor a
direction. The owner applies it to **its own** active objects, then fans out one relay per
overlapped region via `GetOverlappedServers`. Each receiver applies only to objects it owns, so
ownership stays exactly where the handoff protocol put it — §5.1's event relay, not a ghost band.

A new `CommandFlags::AlreadyFannedOut` bit is set on the fanned copies. A receiver applies but does
**not** fan out again; without it one blast would circulate the mesh and objects in a doubly
overlapped region would be pushed twice (2x velocity, silently corrupting any measurement).

**This broke the I4 identity, and the fix is worth recording.** One area command legitimately
applies once per overlapped region, so `sent = applied + rejected + dup` no longer holds — the live
run showed `77 sent` against `154 applied`. Rather than exempt area effects from the invariant, the
fan-out hops are counted separately (`cmdFanout`) and the identity becomes:

```
sent == applied + rejected + duplicate - fanout
```

Verified exactly on a combined run (impulses + misroutes + blasts): `1642 + 14 + 0 - 77 = 1579`
against `cmdSent = 1579`, gap **0**.

### Increment 5 — runtime spawn

- `NetworkIdSpace.h` — bit 30 marks a runtime id, bits 29..22 the origin server, bits 21..0 a
  per-server counter. Static partitioning rather than a manager-issued lease, because putting
  allocation in `SystemManager` would add a round trip per spawn and make the manager an
  availability single point of failure for *gameplay* — which would directly weaken the
  "no central physics bottleneck" claim. 6 unit tests cover round-trip, disjointness, separation
  from pre-seeded ids, and loud failure on exhaustion.
- `SpawnCommand` is point-targeted and **not** special-cased in routing: a spawn on a border is
  simply owned by whoever the half-open rule assigns it to.
- `DistributedObjectSpawnedPacket` is broadcast on the sender server, exactly as the handoff packet
  is. Peers build a **deactivated** twin, the owner an active object, clients a replica — the
  pre-seed model reproduced at runtime, which is what lets handoff work with zero changes.
- `SpawnObject` queues a `PendingSpawn` that the manager drains and broadcasts, mirroring the
  relay queue, because the world manager has no network access.

**No client change was needed.** `SpawnReplica` is already lazy and id-driven, so a runtime object
gets its replica when its first snapshot arrives.

### Verification

| Run | conservation | I5 | I4 | hoFail |
|---|---|---|---|---|
| spawn only | 380 + 59 = 439 = 400 + 39 | 44 = 44 | gap 0 | 0 |
| spawn + blasts | 386 + 53 = 439 | 36 = 36 | gap 0 | 0 |
| spawn + shuttle | 383 + 68 = 451 = 400 + 51 | 93 = 93 | gap 0 | 0 |

Spawns route correctly by owner (20 / 19 across the seam), and 52 deactivated twins were observed
being created on peers.

**Runtime-spawned objects survive a handoff**: the final run recorded **51 handoffs of runtime ids**
(starting at 1073741824 = 2^30, the runtime bit for server 0 counter 0), with `hoFail = 0`. That is
the property the twin broadcast exists for, and it is now demonstrated live rather than argued.

Getting there needed one behavioural addition: a spawned object under the `shuttle` workload now
receives the same deterministic lateral velocity a pre-seeded one does. Without it a spawn simply
fell and settled, so it could never cross a border and the twin path stayed unexercised — three
earlier runs showed 52 twins created but **0** runtime handoffs, which would have read as "verified"
if only the twin count had been checked.

---

## 14. Implementation notes — increment 6 (shipped 2026-08-16)

### The risky change turned out to be avoidable

§4.3 W2 called for making the transition ack **load-bearing**: retain `mNewServerID` until the ack
arrives, so a destroy that lands after the object was released can be forwarded to the new owner.
The spec flags this as one of only two changes in the whole design that alter existing handoff
semantics, and warns that a lost ack would leak transition state.

**It was not needed.** `Destroy` is object-targeted, and a server that has already handed an object
away still holds that object's last known transform — a position which, by definition, lies inside
the **new** owner's region (that is why the handoff happened). So the ordinary relay-on-not-owner
path from increment 3 forwards the destroy to exactly the right server with no protocol change, no
retained state, and no new failure mode. Covered by `DestroyRelaysAfterHandoff` and observed live.

The handoff protocol is therefore **untouched by this increment too** — all of increments 1-6 are
purely additive to it.

### What shipped

- `NetworkObject::CancelPendingTransition()` — race W1. The tick order runs the network pump before
  `HandleObjectTransitions`, so a destroy arriving after the transition flag is set but before
  dispatch is the common case, and destroy wins. A pure local state reset.
- **Tombstones**, kept forever. IDs are never recycled (that would need distributed agreement on
  when every server *and* client has retired one — a distributed GC problem), which is exactly what
  makes a permanent tombstone cheap and safe. The pool entry is nulled rather than erased, so a late
  relayed command resolves to `ObjectDestroyed` rather than `ObjectUnknown`.
- **Deferred teardown.** `SetActive(false)` → `UnregisterObject` → `RemoveGameObject(obj, false)` →
  queue for deletion. Objects are freed at the **end** of the next tick, after `mPhysics->Update`
  has run `FlushPendingUnregisters` and purged every raw pointer to them. My first attempt freed at
  the *top* of `Update`, which is wrong: the physics purge runs later in the same tick, so the
  collision containers would have held a dangling pointer for the rest of it.
- Race W3 — a destroy that beats the object to its new owner is held in
  `mPendingDestroyOnArrival`; `StartHandlingObject` then drops the object instead of activating it.
- Race W4 — destroy is idempotent: a second destroy observes the tombstone and reports success.
- Client-side tombstones plus a resurrection guard in `SpawnReplica` (invariant I3).

### Verification

High-churn reproducible run (7200 ticks, 507 spawns, 758 destroys, 662 handoffs):

| Invariant | Result |
|---|---|
| **I2** conservation | `400 + 507 - 758 = 149` = owned 149 — **exact** |
| **I3** no resurrection | 0 attempts |
| **I4** command accounting | `1265 applied + 1 rejected = 1266` = `cmdSent` — **gap 0** |
| **I5** handoff parity | 662 = 662, `hoFail = 0` |

The hop guard also fired once for real: a destroy relayed to a server that also did not own the
object tried to relay again and was dropped as a routing loop, counted as `cmdRejected` — which is
why I4 still balances.

### Honest limits of the live evidence

Two guards hold but were **not exercised**, and both have a legitimate explanation rather than
being untested by accident:

- **I3's resurrection guard: 0 attempts.** The server tears the object down and broadcasts the
  despawn in the same tick, and snapshots travel the same reliable link, so every snapshot
  containing the object necessarily *precedes* its despawn. The guard is defensive against the
  cross-server case (object handed off, then destroyed by the new owner while the old owner's
  snapshot is still in flight), which this workload does not produce.
- **Race W3: 0 occurrences.** It needs the despawn broadcast to overtake the handoff packet on a
  different link. Possible, but rare enough not to appear in 662 handoffs.

Reaching either deliberately would need fault injection (delayed or reordered links), which is
Tier 2 soak/chaos territory and is not built. Recorded so neither reads as "verified".

---

## 15. Implementation notes — increments 7 and 8 (shipped 2026-08-16)

### Increment 8 — directed peer send and late-join manifest

- `GameServer::SendPacketToPeer(peerNumber, packet)` plus a retained
  `std::map<int, _ENetPeer*> mPeerHandles`. `mPeers` holds peer *numbers*, not handles, so a
  directed send was impossible without this.
- `DistributedPacketSenderServer::RegisterOnPeerJoinedEvent` fires per peer, before the
  all-connected event — a late joiner needs its manifest whether or not it happens to complete the
  set.
- `BuildOwnedObjectManifest()` returns one entry per object this server **owns**; peer-owned and
  tombstoned entries are skipped, so the joiner receives the whole world exactly once across all
  servers. Archetypes are now recorded for pre-seeded objects too, otherwise a joiner would be told
  every existing object is the default shape.
- `StartDistributedGameServerPacket` arrays are clamped to `MAX_SERVERS = 20` with a diagnostic.
  Exceeding them was a buffer overflow, not a truncation.

**A second copy of an already-fixed bug.** `DistributedPacketSenderServer::UpdateServer` duplicates
`GameServer`'s ENet event loop, and that copy still carried the hardcoded `for (i = 0; i < 3; ++i)`
disconnect loop — the exact bug fixed in the base class during Track 0 — **and never decremented
`mClientCount` at all**. This is the server clients actually connect to, so its peer table filled
up permanently over a long run. It also never stored the peer handle, which is why the first
manifest attempt reported `manifestSent=0` while still printing "sent manifest": the sends were
silently finding no destination. Both fixed.

Verified: `manifestSent=800` (400 owned objects x 2 joining peers) with conservation, I4 and I5 all
exact.

### Increment 7 — player-controlled avatars

The **only** wire-format change in the entire interaction design. Two fields are **appended** to
`StartSimulatingObjectPacket` — `mControllerPlayerID` and `mMoveAxis` — so every existing offset is
unchanged.

Movement input is genuinely treated as *state*: `SetMoveAxis` now only records it on the object, and
`ApplyControlForces()` re-applies it every tick before the integrator. Previously the force was
applied once on receipt, which made movement depend on the client's packet rate rather than on the
input. Because it is state and is never relayed or replayed, it has to travel with the object —
hence the appended fields.

Verified with `--drive-every`: one object driven along +X across the seam over 7200 ticks.

| | server 0 | server 1 |
|---|---|---|
| MoveAxis applied | 262 | 2702 |
| handoffs sent / received | 47 / 2 | 2 / 47 |

- **I4 exact:** `2964 applied + 2 rejected = 2966` = `cmdSent`, gap 0.
- **I5 exact:** 49 = 49, `hoFail = 0`. **Conservation exact:** 400.
- The driven object kept moving after crossing (2702 applications on the server it moved to), and
  only **2** commands out of 2966 hit `NotOwner` across 49 handoffs — that residual is the
  crossing window itself, and it is counted rather than lost.

### Status

Increments 1-8 are complete. Of the two changes the design flagged as altering existing semantics,
one (increment 2's border rule) shipped as planned and one (increment 6's load-bearing ack) proved
**unnecessary** — the ordinary relay path covers it. The handoff protocol's behaviour is unchanged
throughout; the single wire change is append-only.

---

## 16. Flagged-issue remediation (2026-08-17)

Four issues were recorded as open at the end of increment 8. Three are fixed; the fourth is
diagnosed and quantified rather than fixed, for the reason given.

### 1. `std::string` members in memcpy'd packets (§0.13) — FIXED

Every live distributed packet now uses fixed-width `char` arrays: `GameStartStatePacket::levelSeed`,
`DistributedPhysicsClientConnectedToManagerPacket::ipAddress`,
`DistributedClientConnectToPhysicsServerPacket::ipAddress`,
`DistributedClientsGameServersAreReadyPacket::ipAddresses`,
`StartDistributedGameServerPacket::createdServerIPs` and
`PhysicsServerMiddlewareConnectedPacket::ipAddress`, filled via a truncating, always-terminating
`CopyToPacketField` helper in `NetworkBase.h`. `DistributedClientsGameServersAreReadyPacket` now
also zeroes its arrays, which it never did — an unset slot previously put stack contents on the
wire. Verified with a full run: conservation, I4 and I5 all exact.

### 2. Increment 2's vacuous verification — FIXED

New `--workload seam` centres each object grid on the world origin, placing a whole row and column
**exactly** on `x = 0` / `z = 0`. Two-server run: pre-seed ownership is `200 + 200 = 400`, exact.
Under the old closed-on-both-axes rule those border objects would have been claimed by both
regions. The check is no longer vacuous.

### 3. A regression this work exposed — FIXED

The increment 8 manifest sent **every owned object** to every joining peer. At bootstrap that is a
~400-entry reliable burst per peer, which flooded the link and stopped a 4-server instance starting
at all (one server never received `GameStartState`). The manifest now carries **runtime-spawned
objects only**: pre-seeded objects need no manifest, because every server builds the identical set
independently and clients learn them from the first snapshot. That is also what §3.5 actually asks
for.

### 4. A missing guard, found by trying to test it

The tombstone check in `StartHandlingObject` — documented in §14 as shipped — **was not in the
code**. A scripted edit had silently failed to apply, so `mPendingDestroyOnArrival` was written but
never read. Restored, and it now also counts the handoff as received so dropping it cannot break
I5. This is a direct argument for fault injection: the guard was documented, believed present, and
absent.

### 5. Races W3 and the resurrection guard — STILL UNEXERCISED, with a reason

New fault injection: `--handoff-delay-ticks N` holds each transfer packet back N ticks while
releasing the object locally at the normal moment, widening the ownership gap on demand. Forwarded
by the midware; **must be 0 for any measurement run**.

Even at 240 ticks (2 s) with 1023 destroys against 992 handoffs, neither path fired. The reason is
structural, not a gap in the testing: **W3 as the spec frames it — "B has no object with that ID" —
cannot occur in this architecture**, because the deactivated-twin design guarantees every server
holds a pool entry for every object. A destroy that reaches the new owner first simply destroys the
twin. `mPendingDestroyOnArrival` is therefore a safety net for a lost spawn broadcast, not a
reachable race.

### What the fault injection did reveal — the ownership gap, quantified

A clean A/B at identical churn (7200 ticks, ~758 spawns, ~1100 destroys):

| | I2 conservation | I5 parity | I4 |
|---|---|---|---|
| no delay | `400 + 758 - 1158 = 0` = owned 0, **exact** | 481 = 481 | gap 0 |
| 240-tick delay | `400 + 755 - 1023 = 132` vs owned **129** | 992 vs **989** | gap 0 |

**Three objects lost, and both invariants miss by exactly three.** Those are the objects still in
flight when the run ended: released by the sender, transfer never delivered. This is §0.7's
ownership gap — the sender deactivates on send and the transfer is unacknowledged — now *measured*
rather than argued. It is the paper's clearest quantification of the protocol's known weakness, and
it is reproducible on demand with one flag.
