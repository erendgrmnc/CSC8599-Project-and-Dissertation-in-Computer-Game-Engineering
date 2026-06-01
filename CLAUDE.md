# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Newcastle University Computer Game Engineering MSc dissertation project (CSC8599): a **distributed physics server system** layered on top of the CSC8503 game engine. The physics simulation of a single shared world is split across multiple server processes by spatial region, with objects handed off between servers as they cross region borders. `NCL` is the engine's root namespace (Newcastle Coursework Library).

## Build

CMake generates a Visual Studio solution (MSVC, x64, C++20). There is no Linux/Make path; the only other target is PS5 (`Prospero`).

```powershell
# From the repo root. Regenerating after editing CMake toggles requires a clean cache.
Remove-Item CMakeCache.txt -ErrorAction SilentlyContinue
cmake -G "Visual Studio 17 2022" -A x64 .
# Then build the startup project (EntryPoint) via the generated solution:
msbuild DistributedPhysicsSystem.sln /p:Configuration=Debug /p:Platform=x64
```

Or open `DistributedPhysicsSystem.sln` in Visual Studio and build (`EntryPoint` is the startup project). Asset paths are baked into the binary at configure time via the `ASSETROOTLOCATION` compile definition pointing at `Assets/`.

There is no automated test suite. Validation is empirical, done by running the roles together and reading the on-screen profilers (see `docs/DissertationEvaluationVisuals/`).

## The build-mode toggle (most important thing to know)

`EntryPoint/main.cpp` is the **single** entry point for every executable. Which role it compiles into is selected entirely by compile definitions set near the top of the root `CMakeLists.txt`:

```cmake
set(CMAKE_DISTRIBUTED_SYSTEM_ACTIVE "true")    # off -> plain CSC8503 game (RunGame)
set(CMAKE_BUILD_FOR_DISTRIBUTED_MANAGER "false")
set(CMAKE_BUILD_FOR_PHYSICS_MIDWARE "true")
```

These map to the `DISTRIBUTEDSYSTEMACTIVE`, `BUILDFORDISTRIBUTEDMANAGER`, and `BUILDFORPHYSICSMIDWARE` preprocessor defines. `main.cpp` `#include`s the matching `ProgramStart.cpp` and calls its start function:

| Toggle state | Role built | Entry fn | Source |
|---|---|---|---|
| MANAGER=true | Distributed Manager (orchestrator) | `StartProgram()` | `DistributedPhysicsManager/ProgramStart.cpp` |
| MIDWARE=true | Physics Server Midware (process launcher) | `StartMidware()` | `PhysicsServerMidware/ProgramStart.cpp` |
| both false | Distributed Game Server (physics sim) | `StartGameServer()` | `DistributedGameServer/ServerStarter.cpp` |
| DISTRIBUTED_SYSTEM_ACTIVE=false | Thin distributed client | `RunDistributedClient()` | `CSC8503/DistributedClientStart.cpp` |

**Only one role builds per configuration.** To switch roles you must edit these CMake variables and regenerate (clean cache). Running the full distributed system therefore means producing several differently-configured builds of the same solution.

## Runtime topology

The four roles form a hierarchy that bootstraps a distributed simulation:

1. **Distributed Manager** (`SystemManager`) — the orchestrator. Listens on port **1234**. The operator enters server count, client count, and objects-per-player at the console, then presses **S** to create a `GameInstance`. The manager computes a spatial `GameBorder` (min/max X and Z) for each physics server, then tells midwares to launch the server processes and tells clients where to connect.
2. **Physics Server Midware** (`ServerMidwareManager`) — runs on each physics machine; connects to the manager and, on receiving a `RunDistributedPhysicsServerInstance` packet, spawns a **Distributed Game Server** process. Launch parameters are passed as a single hyphen-delimited `argv[2]` string: `ip-port-serverID-gameInstanceID-borders` (parsed in `ServerStarter.cpp`).
3. **Distributed Game Server** (`DistributedGameServerManager` + `ServerWorldManager`) — simulates physics for objects inside its assigned border region. When an object leaves the region it performs a transition handshake (`StartSimulatingObjectInServer` / ...`Received`) to hand the object off to the neighbouring server. Each game server also runs a `DistributedPacketSenderServer` that broadcasts world snapshots (delta/full state) to game clients inline in its update loop (the dedicated sender thread is currently commented out).
4. **Thin Client** (`DistributedMultiplayerGameScene`, hosted by `RunDistributedClient` in `CSC8503/DistributedClientStart.cpp`) — connects to the manager to discover instance data, then connects to the relevant physics server(s) to receive snapshots. It is a slim `PacketReceiver` (not part of any scene/level hierarchy).

The interesting domain logic lives in `ServerWorldManager` (border checks, object handoff, `CalculateIncomingObjectOffsetedPosition`) and `SystemManager`/`GameInstance` (border calculation, instance lifecycle).

## Networking layer

Built on **ENet** (`NetworkBase` wraps the opaque `_ENetHost`/`_ENetPeer`; the headers forward-declare these to avoid leaking the ENet include). Wire protocol:

- All packets derive from `GamePacket` (`short type; short size;`). Message types are the `BasicNetworkMessages` enum in `CSC8503CoreClasses/NetworkBase.h` — the distributed-system types live at the bottom of that enum. Shared packet/DTO structs are in `CSC8503CoreClasses/DistributedSystemCommonFiles/`.
- Dispatch is via `PacketReceiver` + `RegisterPacketHandler(msgID, receiver)`, stored in a `multimap`. Managers/servers implement `ReceivePacket(type, payload, source)` and switch on `type`.

Key networking classes (all in `CSC8503CoreClasses/`): `GameServer`/`GameClient` (base ENet wrappers), `DistributedPhysicsManagerServer` (manager's server), `DistributedPhysicsServerClient` (a game server's uplink to the manager), `DistributedPacketSenderServer` (a game server's downlink to clients), and `NetworkObject`/`NetworkState` (per-object state replication).

## Module layout

- `NCLCoreClasses/` — engine foundation: window, input, maths, timer, file loaders.
- `CSC8503CoreClasses/` — game objects, physics (`PhysicsSystem`, collision detection/volumes), networking (above), and the `Distributed*` classes. The team-game gameplay classes (guards, CCTV, doors, level/room loading, `PlayerObject`, animation system, FMOD `SoundObject`) were removed during cleanup; behaviour-tree/pushdown/navigation engine helpers remain.
- `CSC8503/` — now only the thin distributed client: `DistributedMultiplayerGameScene.*` and its host `DistributedClientStart.cpp`. (Previously the team "heist" game app — scenes, UI, inventory/suspicion systems — all removed.)
- `DistributedPhysicsManager/`, `PhysicsServerMidware/`, `DistributedGameServer/` — the three distributed roles (manager + their `ProgramStart`/`ServerStarter`).
- `OpenGLRendering/` — the renderer backend (x64). The Vulkan renderer and PS5/Prospero path were removed.
- `Recast/`, `Detour/`, `DetourTileCache/`, `DebugUtils/` — vendored RecastNavigation nav-mesh library.
- `EntryPoint/` — the shared `main.cpp` and per-role `CMake*.cmake` include files.

Each module owns a `CMakeLists.txt` plus `CMakePC.cmake` listing its sources; add new files to the relevant `CMake*.cmake`, not just to disk.

> **Name lookup note:** `CSC8503CoreClasses/NetworkObject.h` carries a global `using namespace NCL::CSC8503;` that several distributed headers rely on (it was previously pulled in transitively via the now-removed team-game include chain). Several distributed `.cpp` files also add `using namespace NCL;` for the same reason.
