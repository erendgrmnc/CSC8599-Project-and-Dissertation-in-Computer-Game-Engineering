# Networking

The transport, packet protocol, and snapshot replication used by the distributed physics system. For who-talks-to-whom at a higher level, see [ARCHITECTURE.md](ARCHITECTURE.md).

## Foundation: ENet

All transport is **ENet** over UDP. `NetworkBase` wraps the ENet host handle (`_ENetHost`) and is subclassed by the concrete servers/clients (`CSC8503CoreClasses/NetworkBase.h`). Hosts are polled non-blocking each frame.

### Packets

Every message derives from `GamePacket`, a 4-byte header (`NetworkBase.h:63-79`):

```cpp
struct GamePacket {
    short size;   // payload bytes after the header
    short type;   // a BasicNetworkMessages value
};
```

Message type IDs live in the `BasicNetworkMessages` enum (`NetworkBase.h:8-52`); the distributed-system types are appended at the end (`:38-51`).

### Dispatch

Receivers implement `PacketReceiver::ReceivePacket(type, payload, source)` and register themselves per message type:

```cpp
void RegisterPacketHandler(int msgID, PacketReceiver* receiver);  // NetworkBase.h:123
```

Handlers are stored in a `std::multimap<int, PacketReceiver*>`, so multiple receivers can subscribe to one type. When a packet arrives, `ProcessPacket` looks up the handlers for its `type` and forwards it. Each manager (`SystemManager`, `DistributedGameServerManager`, `ServerMidwareManager`, `DistributedMultiplayerGameScene`) registers the subset of types it cares about and switches on `type` in its `ReceivePacket`.

## Endpoints

| Class | Role | Used by |
|---|---|---|
| `GameServer` / `GameClient` | Base ENet server / client wrappers. | Everyone, directly or as a base. |
| `DistributedPhysicsManagerServer` | The manager's listen server on port 1234. | Manager |
| `DistributedPhysicsServerClient` | A game server's **uplink** to the manager. | Game Server |
| `DistributedPacketSenderServer` | A game server's **downlink** that clients subscribe to for snapshots. | Game Server → Clients |

A Game Server therefore holds: one `DistributedPhysicsServerClient` (to the manager), one `DistributedPacketSenderServer` (to clients), and a `GameClient` per peer game server (for object handoff).

## Distributed packet catalogue

The control-plane packets that assemble and run the system (`NetworkBase.h:38-51`; payload structs in `CSC8503CoreClasses/NetworkObject.h` and `DistributedSystemCommonFiles/`). "Originator → recipient":

| Type | Direction | Purpose |
|---|---|---|
| `PhysicsServerMiddlewareConnected` | Midware → Manager | Midware announces itself on connect. |
| `PhysicsServerMiddlewareData` | Manager → Midware | Returns the assigned midware ID. |
| `DistributedClientConnectedToManager` | Client → Manager | Client joins; requests an instance. |
| `DistributedClientGetGameInstanceData` | Manager → Client | Instance ID, player number, counts. |
| `RunDistributedPhysicsServerInstance` | Manager → Midware | Spawn a game server with this serverID/instance/border string. |
| `DistributedPhysicsClientConnectedToManager` | Game Server → Manager | Server registered; carries its packet-sender port + IP. |
| `DistributedClientConnectToPhysicsServer` | Manager → Clients | Tells clients the server IP + packet-sender port to connect to. |
| `StartDistributedPhysicsServer` | Manager → Game Servers | All servers' IDs/ports/borders, so each can build the server mesh. |
| `DistributedPhysicsServerAllClientsAreConnected` | Game Server → Manager | Server's clients are all connected; ready to start. |
| `GameStartState` | Manager → all | Begin the simulation. |
| `StartSimulatingObjectInServer` | Game Server → Game Server | Hand an object (with full physics state) to a neighbour. |
| `StartSimulatingObjectInServerReceived` | Game Server → Game Server | Acknowledge a handoff so the sender can release the object. |

The handoff pair is covered in [SPATIAL-PARTITIONING.md](SPATIAL-PARTITIONING.md#object-handoff-handshake).

## Snapshot replication

World state is replicated per object via `NetworkObject` / `NetworkState`. Two packet shapes are broadcast on the packet-sender server (`NetworkObject.h:12-34`):

```cpp
struct FullPacket : GamePacket {     // type = Full_State
    int objectID;
    int serverID;
    NetworkState fullState;          // full transform + state ID
};

struct DeltaPacket : GamePacket {    // type = Delta_State
    int  fullID;                     // the full state this delta is relative to
    int  objectID;
    int  serverID;
    char pos[3];                     // quantised position delta
    char orientation[4];             // quantised orientation delta
};
```

### Cadence

In `DistributedGameServerManager::UpdateGameServerManager` (`:100-124`): while the game is running, a packet timer counts down; each time it fires the server sends a snapshot and re-arms the timer with `1.0f/60.f` (annotated in code as the "20hz server/client update"). A counter (`mPacketsToSnapshot`) makes **every 6th send a full state** (`BroadcastSnapshot(false)`) and the **five in between deltas** (`BroadcastSnapshot(true)`). Full-state and delta send durations are recorded to the profiler (`Profiler::SetLastFullSnapshotTime` / `SetLastDeltaSnapshotTime`).

A delta is only meaningful relative to a known full state, so it carries `fullID`. Clients track the latest full state they have; the server prunes acknowledged state history via `UpdateMinimumState` once clients have caught up.

### Threading

A `std::mutex mPacketToSendQueueMutex` guards the outgoing packet queue (`DistributedGameServerManager.h:80-82`). The intended design was a dedicated `SendPacketsThread` consuming that queue, but it is currently **commented out** (`DistributedGameServerManager.cpp:69-70`), so broadcasting runs inline on the main update loop. ENet polling itself is non-blocking, so the single-threaded loop services the network each frame.

## Next

- [SPATIAL-PARTITIONING.md](SPATIAL-PARTITIONING.md) — regions and the object-handoff handshake.
- [ARCHITECTURE.md](ARCHITECTURE.md) — where each packet sits in the bootstrap sequence.
