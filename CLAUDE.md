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

`EntryPoint/main.cpp` is the **single** entry point for every executable; which role it compiles into is chosen by preprocessor defines. There is now **one** toggle in the root `CMakeLists.txt`:

```cmake
set(CMAKE_DISTRIBUTED_SYSTEM_ACTIVE "true")   # false -> the non-distributed client / demo build
```

That is the only setting that changes *library* code — it gates ~16 files via `DISTRIBUTEDSYSTEMACTIVE`. Role selection among the three server roles is a **per-target** compile definition applied to the `EntryPoint*` executables in `EntryPoint/CMakeDistributedRoles.cmake`, because `BUILDFORDISTRIBUTEDMANAGER` and `BUILDFORPHYSICSMIDWARE` are consumed in exactly one place: `main.cpp`, where they select which `ProgramStart.cpp` is `#include`d.

| Configure | CMake target | Role | Entry fn | Source |
|---|---|---|---|---|
| ACTIVE=true | `EntryPointManager` | Distributed Manager (orchestrator) | `StartProgram()` | `DistributedPhysicsManager/ProgramStart.cpp` |
| ACTIVE=true | `EntryPointMidware` | Physics Server Midware (process launcher) | `StartMidware()` | `PhysicsServerMidware/ProgramStart.cpp` |
| ACTIVE=true | `EntryPointServer` | Distributed Game Server (physics sim) | `StartGameServer()` | `DistributedGameServer/ServerStarter.cpp` |
| ACTIVE=false | `EntryPoint` | Thin distributed client | `RunDistributedClient()` | `CSC8503/DistributedClientStart.cpp` |

**One configure builds all three server roles at once.** They share byte-identical libraries and differ only by that one define, so `msbuild ... /t:EntryPointManager;EntryPointMidware;EntryPointServer` produces all three. Only the client needs a second configure, because it flips `DISTRIBUTEDSYSTEMACTIVE`.

> This replaced a three-toggle scheme where the role selectors were global `add_compile_definitions`. That forced a clean-cache regenerate and a **full solution rebuild per role** — four rebuilds to produce four binaries, three of which linked identical libraries. If you see `CMAKE_BUILD_FOR_DISTRIBUTED_MANAGER` or `CMAKE_BUILD_FOR_PHYSICS_MIDWARE` referenced anywhere, it is stale. The old per-role `EntryPoint/CMakeDistributedServerManager.cmake`, `CMakeDistributedGameServer.cmake` and `CMakePhysicsServerMidware.cmake` are retained but no longer included.

## Runtime topology

The four roles form a hierarchy that bootstraps a distributed simulation:

1. **Distributed Manager** (`SystemManager`) — the orchestrator. Listens on port **1234**. The operator enters server count, client count, and objects-per-player at the console, then presses **S** to create a `GameInstance`. The manager computes a spatial `GameBorder` (min/max X and Z) for each physics server, then tells midwares to launch the server processes and tells clients where to connect.
2. **Physics Server Midware** (`ServerMidwareManager`) — runs on each physics machine; connects to the manager and, on receiving a `RunDistributedPhysicsServerInstance` packet, spawns a **Distributed Game Server** process. Launch parameters are passed as a single hyphen-delimited `argv[2]` string: `ip-port-serverID-gameInstanceID-borders` (parsed in `ServerStarter.cpp`).
3. **Distributed Game Server** (`DistributedGameServerManager` + `ServerWorldManager`) — simulates physics for objects inside its assigned border region. When an object leaves the region it performs a transition handshake (`StartSimulatingObjectInServer` / ...`Received`) to hand the object off to the neighbouring server. Each game server also runs a `DistributedPacketSenderServer` that broadcasts world snapshots (delta/full state) to game clients inline in its update loop (the dedicated sender thread is currently commented out).
4. **Thin Client** (`DistributedMultiplayerGameScene`, hosted by `RunDistributedClient` in `CSC8503/DistributedClientStart.cpp`) — connects to the manager to discover instance data, then connects to the relevant physics server(s) to receive snapshots. The scene itself is a slim `PacketReceiver` (not part of any scene/level hierarchy); rendering is bolted on by the host loop, which applies snapshots to replica cubes. Two render paths:
   - **default:** `DistributedClientRenderer` — a minimal flat-shaded forward renderer (single shared cube mesh, no textures, no bindless handles). Safe to drive without a level.
   - **`--render-deferred`:** the full `GameTechRenderer`. **Experimental** — driven without a level it can issue invalid GPU work and hang the display. Don't make it the default.

   `DistributedClientOverlay` draws the colour-coded server-region grid + legend (bespoke `overlayLine`/`overlayText` shaders, classic `sampler2D`, deliberately *not* the engine's bindless `DebugText` path). **F3** toggles it; it re-emits `Debug` primitives every frame.

The interesting domain logic lives in `ServerWorldManager` (border checks, object handoff) and `SystemManager`/`GameInstance` (border calculation, instance lifecycle).

> **Verified-state warnings (audit, Aug 2026).** Several things the docs describe as working are not. Check before relying on them:
> - `CalculateIncomingObjectOffsetPosition` (`ServerWorldManager.cpp:298`) — note the name has no "ed"; `docs/SPATIAL-PARTITIONING.md` describes it as live. It is **dead code**: never called, X branches commented out, and it returns a reference to a stack local.
> - The **handoff ack is stubbed**. `ServerWorldManager::HandleTransitionHandshakeReceived` has an empty body; the sender-side handler body is commented out; `NetworkObject::OnTransitionHandshakeReceived` is never called. The sender deactivates the object immediately on send, so a dropped packet loses the object permanently, and there is an ownership gap where no server broadcasts it.
> - **Snapshots are sent at 60 Hz** (1 full : 5 delta → 10 Hz full, 50 Hz delta). The inline comment and `docs/NETWORKING.md` both say 20 Hz — both are wrong.
> - **Deltas never apply after the first full snapshot.** `mServerSideLastFullID` is only written in `HandleClientPlayerInputPacket`, which never fires, so every delta carries `fullID=0` while the client's `stateID` advances. The client runs on 10 Hz full snapshots; the delta traffic is generated, sent and discarded.
> - **The integrator ignores ownership.** `IntegrateAccel`/`IntegrateVelocity` null-check only — they do not test `HasPhysics()`/`IsNetworkActive()`, so every server integrates every object in the world, not just its region's.
> - **The timestep is not fixed.** `realHZ`/`realDT` are file-scope globals mutated at runtime on overrun/underrun, so servers under different load run different timesteps.
> - **There is no cross-border collision.** Deactivated out-of-region objects are skipped by broadphase; there is no ghost/halo band. Objects on opposite sides of a boundary pass through each other.
> - `docs/NETWORKING.md` and `docs/SPATIAL-PARTITIONING.md` are otherwise faithful on control flow, but are also wrong that world bounds are fixed at ±150 (now `--world`).

## Networking layer

Built on **ENet** (`NetworkBase` wraps the opaque `_ENetHost`/`_ENetPeer`; the headers forward-declare these to avoid leaking the ENet include). Wire protocol:

- All packets derive from `GamePacket` (`short type; short size;`). Message types are the `BasicNetworkMessages` enum in `CSC8503CoreClasses/NetworkBase.h` — the distributed-system types live at the bottom of that enum. Shared packet/DTO structs are in `CSC8503CoreClasses/DistributedSystemCommonFiles/`.
- Dispatch is via `PacketReceiver` + `RegisterPacketHandler(msgID, receiver)`, stored in a `multimap`. Managers/servers implement `ReceivePacket(type, payload, source)` and switch on `type`.

Key networking classes (all in `CSC8503CoreClasses/`): `GameServer`/`GameClient` (base ENet wrappers), `DistributedPhysicsManagerServer` (manager's server), `DistributedPhysicsServerClient` (a game server's uplink to the manager), `DistributedPacketSenderServer` (a game server's downlink to clients), and `NetworkObject`/`NetworkState` (per-object state replication).

## Module layout

- `NCLCoreClasses/` — engine foundation: window, input, maths, timer, file loaders.
- `CSC8503CoreClasses/` — game objects, physics (`PhysicsSystem`, collision detection/volumes), networking (above), and the `Distributed*` classes. The team-game gameplay classes (guards, CCTV, doors, vents, `PlayerObject`, inventory/suspicion) were removed during cleanup. The **generic engine systems** were later restored, decoupled from that gameplay, for future demo-assessment use: level/room loading (`Level`, `Room`, `JsonParser`, `LevelEnums`), `AnimationSystem`, FMOD `SoundObject`, and the `RecastBuilder` nav-mesh generator. Behaviour-tree/pushdown/navigation engine helpers also remain.
- `CSC8503/` — the thin distributed client (`DistributedMultiplayerGameScene.*`, its host `DistributedClientStart.cpp`, `DistributedClientRenderer`, `DistributedClientOverlay`), plus the restored `GameTechRenderer` and its imgui UI wrappers (`BaseUI`, `WindowsUI`) for demo builds. (Previously the team "heist" game app — scenes, inventory/suspicion systems — all removed.)
- **Demo-only guard convention:** restored renderer / animation / sound files are wrapped in `#ifndef DISTRIBUTEDSYSTEMACTIVE`. The three server roles define `DISTRIBUTEDSYSTEMACTIVE`, so those files compile to empty objects in the lean servers; the non-distributed build does not define it and compiles them fully. Note the client is that same non-distributed build, so the guard gates **both** the demo-assessment app and the client's rendering/overlay — a client-render change lands inside `#ifndef DISTRIBUTEDSYSTEMACTIVE` blocks, and the headless client path above them must keep working without any of it. FMOD include/link/DLL-copy lives only in `CSC8503/` and `EntryPoint/` CMake, never in the server-linked `CSC8503CoreClasses`. The level-loading cluster and `RecastBuilder` are pure-data after decoupling and stay unguarded.
- `DistributedPhysicsManager/`, `PhysicsServerMidware/`, `DistributedGameServer/` — the three distributed roles (manager + their `ProgramStart`/`ServerStarter`).
- `OpenGLRendering/` — the renderer backend (x64). The Vulkan renderer and PS5/Prospero path were removed.
- `Recast/`, `Detour/`, `DetourTileCache/`, `DebugUtils/` — vendored RecastNavigation nav-mesh library.
- `EntryPoint/` — the shared `main.cpp` and per-role `CMake*.cmake` include files.
- `tools/` — deployment tooling (outside CMake). See `tools/README.md` and the *Running the system* section below.

Each module owns a `CMakeLists.txt` plus `CMakePC.cmake` listing its sources; add new files to the relevant `CMake*.cmake`, not just to disk.

> **Name lookup note:** `CSC8503CoreClasses/NetworkObject.h` carries a global `using namespace NCL::CSC8503;` that several distributed headers rely on (it was previously pulled in transitively via the now-removed team-game include chain). Several distributed `.cpp` files also add `using namespace NCL;` for the same reason.

## Running the system

Don't flip CMake toggles by hand to bring up a run. `tools/build-deploy.ps1` does all four builds and stages them:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1   # restores the original toggle when done
dotnet run --project tools\DistributedLauncher\DistributedLauncher.csproj
```

```
deploy/Manager|Midware|Client|DistributedPhysicsServer/EntryPoint.exe
```

The midware spawns `./DistributedPhysicsServer/EntryPoint.exe` **relative to its own working directory** (`PHYSICS_SERVER_PATH` in `ServerMidwareManager.cpp`) unless overridden with `--server-exe`; a "midware connects but no servers appear" failure is usually this path.

**Flags** (parsed by `LaunchConfig`; with *no* flags every role falls back to its original `std::cin` prompts, so both paths must keep working when you touch a `ProgramStart`):

| Role | Flags |
|---|---|
| Manager | `--servers N --clients N --objects N --port P --world minX,maxX,minZ,maxZ --midwares N --autostart [--headless]` |
| Midware | `--manager-ip A.B.C.D --manager-port P --server-exe <path> [--headless] [--fixed-step] [--seed N]` |
| Game Server | `--headless`, `--fixed-step`, `--seed N` — **not passed directly**, see below |
| Client | `--manager-ip A.B.C.D --manager-port P [--game-instance N] [--render-deferred]` |

> **Game servers are spawned by the midware, not the launcher.** Their launch string is built in `ServerMidwareManager::StartPhysicsServerInstance`, so a flag the game server understands is unreachable unless the midware forwards it. `--fixed-step` and `--seed` are therefore given to the **midware**, which appends them to every server it spawns (`mServerExtraArgs`). Any new game-server flag needs adding in both `ServerStarter.cpp` (to parse it) and `PhysicsServerMidware/ProgramStart.cpp` (to forward it) — otherwise it is silently ignored with no error.
>
> `--fixed-step` pins the physics substep rate (otherwise `mRealHZ`/`mRealDT` adapt to measured frame cost, so servers under different load integrate with different `dt`). `--seed` drives deterministic world construction. **Both are required for any measurement run whose numbers are meant to be comparable.**

**Run model:** `--headless` swaps the OpenGL `ProfilerRenderer` window for a `GameTimer` loop (`DistributedSystemCommonFiles/HeadlessRunner`). Every role prints a `@@STAT role=... key=val ...` line to stdout ~2 Hz (`TelemetryReporter`); a headless midware spawns its game servers windowless (pipe-redirected, no `CREATE_NEW_CONSOLE`) and forwards their stdout tagged `[server N]`. The launcher parses both into a live dashboard + per-entity log tabs, so a headless run shows **one window**. Clients always run windowed. Untick **Headless** for the per-role profiler windows used in evaluation visuals.

## Reference docs

Read these before re-deriving behaviour from source:

| Doc | Contents |
|---|---|
| `docs/SETUP.md` | Prerequisites, the toggle matrix, building each role. |
| `docs/DEPLOY.md` | Ports, the launch string, single-machine and multi-PC bring-up. |
| `docs/ARCHITECTURE.md` | The four roles and the end-to-end bootstrap sequence. |
| `docs/NETWORKING.md` | Packet catalogue, snapshot delta/full replication, threading. |
| `docs/SPATIAL-PARTITIONING.md` | Region borders and the object-handoff handshake. |
| `docs/superpowers/specs/` | Dated design specs for recent features, each with a post-build "implementation notes" section recording what shipped and what was deliberately deferred. |
