# Spatial Partitioning & Object Handoff

How the world is divided into per-server regions and how objects move between servers. This is the core of the dissertation. For the packets involved, see [NETWORKING.md](NETWORKING.md).

## The world and its regions

The simulated world is a fixed square on the X/Z plane, **−150 to +150 on each axis** (`CSC8503CoreClasses/DistributedSystemCommonFiles/DistributedPhysicsServerDto.cpp:6-10`). The manager divides this square into one rectangular **region per server**.

### Border algorithm

`GameInstance::CalculateServerBorders` (`DistributedPhysicsServerDto.cpp:91-135`) lays servers out in a grid:

- `numCols = ceil(sqrt(serverCount))`, `numRows = ceil(serverCount / numCols)`.
- Each region is `300/numCols` wide by `300/numRows` tall.
- Server `i` takes `row = i / numCols`, `col = i % numCols`; the right/bottom edge servers extend to the world max so there are no gaps.
- **Special case:** for exactly **3 servers**, a 2×2 grid is used where server 2 (the bottom row) spans the full width (`:100-104`, `:126-132`).

### Border encoding

A region (`GameBorder`: `minX, maxX, minZ, maxZ`) is serialised to a string as **`minX/maxX|minZ/maxZ`** (`GameInstance::GetServerAreaString`, `DistributedPhysicsServerDto.cpp:84-89`). That string is what travels in the `RunDistributedPhysicsServerInstance` packet and the game-server [launch string](DEPLOY.md#the-game-server-launch-string). On the server side it is parsed back into `PhyscisServerBorderData` (`int minXVal/maxXVal/minZVal/maxZVal`) by `DistributedGameServerManager::CreatePhysicsServerBorders`.

Each game server knows **its own** region (`mServerBorderData`) **and every server's** region (`mServerBorderMap`), the latter so it can decide *which* neighbour an exiting object belongs to.

## Object ownership and pre-seeding

When objects are created, every server builds the **same** object set into a pool keyed by network ID (`mCreatedObjectPool`, `ServerWorldManager::CreateObjectGrid:243-253`), but only **activates** the objects that fall inside its own region (`IsObjectInBorder`); objects outside are created and immediately deactivated (`SetActive(false)`).

This pre-seeding is what makes handoff cheap: a receiving server does not instantiate a new object, it **reactivates the one already sitting in its pool**.

### Boundary tests

- `IsObjectInBorder(pos)` — is a position inside *this* server's region (`ServerWorldManager.cpp:272-280`; half-open on X: `>= minX && < maxX`, inclusive on Z).
- `GetObjectServer(pos)` — scan `mServerBorderMap` and return the server ID whose region contains the position, or `-1` (`:282-294`).

## Object handoff handshake

Each tick (when the game is running) the owning server checks its active objects and migrates any that have left its region.

**1 — Detect exit.** `CheckPositionOutOfServerBoundries` (`ServerWorldManager.cpp:146-161`) walks active networked objects; if an object's current position now maps to a *different* server (`GetObjectServer != mServerID`), it flags the object for transition via `networkComp->FinishTransitionToNewServer(newServer)` (records the target server ID on the `NetworkObject`).

**2 — Send the object + release it.** `HandleObjectTransitions` (`DistributedGameServerManager.cpp:444-453`) then, for each flagged object:
  - builds a `StartSimulatingObjectPacket` carrying the object ID, target server ID, sender server ID, the latest `NetworkState` (with current position/orientation), **and the full physics state** (linear/angular velocity, force, etc. — `SendFinishTransactionPacket:455-465`),
  - broadcasts it reliably on the packet-sender server (`SendGlobalReliablePacket`),
  - and **immediately releases the object locally** (`HandleTransitionComplete` + `ServerWorldManager::HandleOutgoingObject`, which deactivates it and drops it from the active list).

  > The sender hands off *eagerly* — it does not wait for an acknowledgement before deactivating.

**3 — Receive and resume.** The target server receives `StartSimulatingObjectInServer` and calls `ServerWorldManager::StartHandlingObject` (`:163-203`): it looks the object up in its pool by ID, restores the network state and the velocities/force from the packet, repositions it, and reactivates it (`SetActive(true)`, `SetServerID(mServerID)`). From the next tick it is simulated locally and replicated to that server's clients.

**4 — Acknowledgement (scaffolded).** A return handshake exists — `StartSimulatingObjectReceivedPacket` sent via `SendTransactionHandshakePacket` (`:467-478`) and handled by `HandleTransitionHandshakePacketReceived` (`:310-319`) — routed to the original sender over the server-to-server `GameClient` mesh. In the current code this path is **partly stubbed**: `StartHandlingObject` marks sending the ack as a `TODO` (`ServerWorldManager.cpp:192`), and the receive handler's cleanup is commented out (`:314`) because the sender already released the object in step 2. So in practice migration is effectively one-way (send-and-release); the ack scaffolding is in place for a future two-phase confirmation.

### Incoming position offset

`CalculateIncomingObjectOffsetedPosition` (`ServerWorldManager.cpp:296-314`) exists to nudge a handed-off object just inside the receiving region's bounds (so it isn't re-detected as out-of-bounds on arrival). Most of its branches are currently commented out, leaving only a Z-axis floor adjustment active — another area flagged for refinement.

## Summary

| Concept | Where |
|---|---|
| World bounds (±150 X/Z) | `DistributedPhysicsServerDto.cpp:6-10` |
| Grid border algorithm | `DistributedPhysicsServerDto.cpp:91-135` |
| Border string `minX/maxX\|minZ/maxZ` | `DistributedPhysicsServerDto.cpp:84-89` |
| Exit detection | `ServerWorldManager::CheckPositionOutOfServerBoundries` |
| Send + release | `DistributedGameServerManager::HandleObjectTransitions` / `SendFinishTransactionPacket` |
| Resume on target | `ServerWorldManager::StartHandlingObject` |
| Ack (scaffolded/TODO) | `SendTransactionHandshakePacket` / `HandleTransitionHandshakePacketReceived` |

## Next

- [NETWORKING.md](NETWORKING.md) — the handoff packets and snapshot replication.
- [ARCHITECTURE.md](ARCHITECTURE.md) — where handoff sits in the running system.
