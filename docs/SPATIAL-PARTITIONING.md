# Spatial Partitioning & Object Handoff

How the world is divided into per-server regions and how objects move between servers. This is the core of the dissertation. For the packets involved, see [NETWORKING.md](NETWORKING.md).

## The world and its regions

The simulated world is a square on the X/Z plane whose extent is set at runtime by the manager's `--world minX,maxX,minZ,maxZ` flag (default `-150,150,-150,150`, parsed in `DistributedPhysicsManager/ProgramStart.cpp`). It was formerly a fixed ±150 square, and experiments that grow the world with the server count — E1's locality sweep — depend on it no longer being fixed. The manager divides this square into one rectangular **region per server**.

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

**4 — Acknowledgement (live).** The receiver acknowledges on acceptance, and the sender holds the transfer packet in custody until that acknowledgement arrives, resending it on a wall-clock deadline (`CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h`, `ServerWorldManager::FlushPendingTransfers`). It reclaims the object only once the peer link itself is gone — never on a bare timeout, because a timeout cannot distinguish "the receiver never got it" from "the receiver got it and is slow". An outstanding transfer is visible as `hoCustody` rather than silently lost.

This paragraph previously described the path as scaffolded, with the ack a `TODO` and the receive handler commented out. That was true before the handoff-custody work; see `docs/superpowers/results/2026-08-20-B-custody.md`.

**Note this does not close the ownership gap.** Custody makes an individual transfer lossless; it does not make ownership transfer atomic. At `--handoff-lookahead 0`, the default, the sender still releases on send and nobody owns the object for one network round trip. See `docs/EVALUATION.md` §6.

### Incoming position offset

`CalculateIncomingObjectOffsetPosition` (`ServerWorldManager.cpp:440` — note the name has no "ed") exists to nudge a handed-off object just inside the receiving region's bounds (so it isn't re-detected as out-of-bounds on arrival). It **is** called now, from `ApplyIncomingObject` on every non-reclaim arrival, but only to observe: the clamp is computed and the result is *discarded*, incrementing `hoClamp` when it would have moved the object. So incoming handoffs still get no positional nudge; what changed is that the codebase now measures how often one would be needed — and that measurement shows the case is reachable, including on the correctness-budget configuration when `--halo-width` is on. See `docs/EVALUATION.md` §7 item 7 and the verified-state warnings in `CLAUDE.md`.

## Summary

| Concept | Where |
|---|---|
| World bounds (`--world`, default ±150 X/Z) | `DistributedPhysicsManager/ProgramStart.cpp` |
| Grid border algorithm | `DistributedPhysicsServerDto.cpp:91-135` |
| Border string `minX/maxX\|minZ/maxZ` | `DistributedPhysicsServerDto.cpp:84-89` |
| Exit detection | `ServerWorldManager::CheckPositionOutOfServerBoundries` |
| Send + release | `DistributedGameServerManager::HandleObjectTransitions` / `SendFinishTransactionPacket` |
| Resume on target | `ServerWorldManager::StartHandlingObject` |
| Ack (live, custody-backed) | `SendTransactionHandshakePacket` / `HandleTransitionHandshakePacketReceived` / `HandoffCustody.h` |

## Next

- [NETWORKING.md](NETWORKING.md) — the handoff packets and snapshot replication.
- [ARCHITECTURE.md](ARCHITECTURE.md) — where handoff sits in the running system.
