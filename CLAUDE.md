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

Validation is largely empirical — run the roles together and read the on-screen profilers (see `docs/DissertationEvaluationVisuals/`). There is one automated suite, `tools/InteractionTests`, a dependency-free assert harness covering pure logic and the physics system:

```powershell
# Note the solution-folder prefix: the target lives under "Tools".
msbuild DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
.\tools\InteractionTests\Debug\InteractionTests.exe   # non-zero exit = failures
```

It links the same libraries as `EntryPointServer` and needs the roles' `target_precompile_headers` list — without it `PhysicsSystem.h` fails on `std::set` and `PhysicsObject.h` on `Matrix3`. Note `AABBVolume` inherits `CollisionVolume` **privately**, so constructing one requires the same `(CollisionVolume*)` cast the engine uses everywhere.

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
> - `CalculateIncomingObjectOffsetPosition` (`ServerWorldManager.cpp:440`) — note the name has no "ed"; `docs/SPATIAL-PARTITIONING.md` describes it as live. It is **still never called**, so incoming handoffs get no nudge. The body itself is now correct (it clamps into the region using bounds that mirror `IsObjectInBorder`); previously it returned a stack local by reference, commented out both X branches, and floored Z identically in both directions. Wiring it up moves incoming objects and so changes measured handoff behaviour — that belongs with the ownership unification, not as a drive-by.
> - The **handoff ack is still stubbed** - `ServerWorldManager::HandleTransitionHandshakeReceived` has an empty body and `NetworkObject::OnTransitionHandshakeReceived` is never called - so a dropped transfer packet still loses the object. The **ownership gap it used to open is closed**: the sender no longer releases on send, it releases on the same `senderTick + lookahead` the receiver installs on, so ownership changes atomically. Before that fix, 1,397 of 1,800 ticks on a uniform run had an object owned by nobody, up to 33 at once, while `conservation_delta` and `ho_parity_delta` both read 0 - end-of-run totals cannot see it. `analyse.py` now sums `owned_objects` per tick and fails on a gap or a double-owner.
> - **Snapshots are sent at 60 Hz** (1 full : 5 delta → 10 Hz full, 50 Hz delta). The inline comment and `docs/NETWORKING.md` both say 20 Hz — both are wrong.
> - **Deltas apply.** `mServerSideLastFullID` is written from the minimum acknowledged
>   state across connected clients (`DistributedGameServerManager.cpp:293`), so the
>   delta baseline advances and deltas are applied rather than discarded. This
>   corrects an earlier warning here that said they never applied - true before the
>   snapshot-acknowledgement work, and not since.
> - **The integrator respects ownership.** `IntegrateAccel`/`IntegrateVelocity` skip objects without physics and skip halo shadows, so a server integrates exactly the objects it owns (invariant I7). Since the region-local increment it holds only those anyway.
> - **The timestep is not fixed** without `--fixed-step`. `mRealHZ`/`mRealDT` adapt to measured frame cost, so servers under different load run different timesteps. (They were file-scope globals shared by every `PhysicsSystem` in the process; now members.)
> - **Runtime object creation is supported by the physics system but unused.** `PhysicsSystem::RegisterObject`/`UnregisterObject` exist and are unit-tested, but no distributed code calls them — there is nothing to spawn or destroy until the interaction increments land. Before this, an object added after the first tick was never integrated *and* never predicted, so it would never have been handed off.
> - **Ownership is now one rule in one place.** `DistributedSystemCommonFiles/RegionOwnership.h` holds `OwningServerFor()`: regions half-open on both axes, `[minX, maxX) × [minZ, maxZ)`, world outer edge closed. `GetObjectServer`, `IsObjectInBorder` and the client's `ResolveCommandTarget` all delegate to it — do not reintroduce a local border test. Previously `IsObjectInBorder` was half-open on X but *closed* on Z while `GetObjectServer` was closed on both, so a point on a shared seam could be claimed by the handoff path and rejected by the pre-seed path (or, on a 4-server grid, claimed by two servers at once).
> - **Cross-border collision now works, behind `--halo-width`.** Each server publishes the objects within the band to the neighbours whose regions they are near; those neighbours hold read-only *halo shadows* that collide but are never integrated (`GameObject::IsHaloShadow`). With the flag at 0 - the default, and how every measurement before it ran - the old behaviour is unchanged and objects pass through each other at a boundary. See `docs/superpowers/specs/2026-08-18-halo-band-cross-border-collision.md`; `--workload headon` is the acceptance test.
> - `docs/NETWORKING.md` and `docs/SPATIAL-PARTITIONING.md` are otherwise faithful on control flow, but are also wrong that world bounds are fixed at ±150 (now `--world`).

## Interaction command channel

Clients issue interactions (push an object, drive it along an axis) through **one** packet shape,
`DistributedClientCommandPacket`. The payload is interpreted by an `IInteractionCommand` looked up
in `CommandRegistry` by `CommandType`, so **adding a new interaction adds zero message types and
touches zero switch statements** — write the command class, register it in `RegisterDefaultsInto`.

- Types live in `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h`, deliberately
  free of `USEGL`/`DISTRIBUTEDSYSTEMACTIVE` guards because the servers, the client and the test
  target all include it. `CommandArgs` must stay POD — packets are `memcpy`'d.

> **Bootstrap readiness is re-checked, not edge-triggered.** A game server starts when its
> `DistributedPacketSenderServer` has all its expected peers, but the expected count only arrives
> with the manager's start packet (`SetMaxClients`) — until then the bound sits at its constructor
> value of 19. The last peer routinely connects *before* that, so the test must be `>=`, one-shot,
> and re-evaluated whenever the bound changes; an `==` check inside `AddPeer` alone gets stepped
> over and that server never starts. `SetMaxClients` also recomputes `mClientCount`, since shrinking
> the bound drops peers past the new end.
- **Authority:** the client routes to whichever server it believes owns the object (`mObjectOwner`,
  maintained from snapshots). A server that does not own it **relays** to the true owner rather
  than rejecting; only the server that *applied* the command acks, so the client gets exactly one
  ack. A `NotOwner` ack carries `correctedServerID`, which the client adopts.
- **Dedupe** is by `(playerID, sequence)` via `SequenceWindow`: reliability does not compose across
  a relay hop. Continuous commands (`MoveAxis`) are idempotent state — never sequenced, never
  relayed, dropped if not owned.
- `ServerWorldManager` implements `ICommandContext`, so commands never see the network layer and
  the network layer never sees the world. `SpawnObject`/`DestroyObject` are **stubs** until the
  spawn/destroy increments land.
- `CommandRegistry::RegisterDefaults()` must be called on the **client too**, not just servers.
- Accounting (invariant I4): `cmdApplied + cmdRejected + cmdDup` summed across servers must equal
  the client's `cmdSent`; `cmdRelayed` is an internal hop counted separately. Compare *aligned*
  2 Hz samples — the client keeps sending after the servers take their last one.
**Runtime spawn and destroy.** Runtime object ids come from `NetworkIdSpace.h` — bit 30 marks a runtime id, bits 29..22 the origin server, 21..0 a per-server counter, so no server can mint another's id and no central allocator is needed. A spawn is still **broadcast**, but only because clients need it to build a replica: peers take just the owner id off it and build nothing, because handoff constructs on arrival. Nothing anywhere holds a deactivated twin any more. Destroy leaves a **permanent tombstone** (ids are never recycled) with the pool entry nulled rather than erased, so a late command resolves to `ObjectDestroyed` rather than `ObjectUnknown`. Objects are freed at the **end** of the next tick, after `mPhysics->Update` has purged the collision sets — freeing earlier leaves a dangling pointer in them for the rest of the tick.

**Late-join manifest.** `GameServer::SendPacketToPeer` (backed by retained `ENetPeer*` handles) plus `RegisterOnPeerJoinedEvent` send a joining peer one spawned-packet per object the server owns. Note `DistributedPacketSenderServer::UpdateServer` **duplicates** `GameServer`'s ENet event loop — a fix in one is not a fix in the other, which is how a hardcoded `i < 3` disconnect loop survived there long after the base class was corrected.

**Avatars.** Movement input is *state*, not an event: `SetMoveAxis` records it and `ApplyControlForces()` re-applies it every tick before the integrator. `mControllerPlayerID`/`mMoveAxis` are **appended** to `StartSimulatingObjectPacket` — the only wire-format change in the interaction design — so control survives a handoff instead of stalling until the client's next update.

- A headless client has no input path, so the channel is only exercised with `--impulse-test N`;
  `--misroute-every N` additionally forces the relay path by sending every Nth command to a server
  that does not own the object. Both roles print an exact `@@FINAL` line at the end of a bounded
  run — use those, not the 2 Hz samples, for invariant checks.

> **Peer links are labelled by server id, not array index.** `StartDistributedGameServerPacket`
> carries two differently-indexed families: `serverIDs[]`/`borders[]` are indexed **by server id**
> (length `totalServerCount`), while `serverPorts[]`/`createdServerIPs[]` are in **registration
> order** (length `currentServerCount`). `connectedServerIDs[]` maps the second family back to real
> ids and is what `HandleStartGameServerPacketReceived` must use. Using the index instead mislabels
> every peer link whenever servers register out of id order, which silently breaks *both*
> `DrainPendingRelays` and `SendTransactionHandshakePacket` — the latter being one concrete reason
> the transition ack never worked.

## Networking layer

Built on **ENet** (`NetworkBase` wraps the opaque `_ENetHost`/`_ENetPeer`; the headers forward-declare these to avoid leaking the ENet include). Wire protocol:

- **Every packet is strict POD with fixed-size arrays.** The ENet path `memcpy`s the struct, so a `std::string` member only ever worked because short-string optimisation kept the bytes inline *and* both ends were the same MSVC x64 binary. Use `CopyToPacketField` (`NetworkBase.h`) to fill a `char[N]` field; new packets get a `static_assert(std::is_trivially_copyable_v<T>)`.
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
| Manager | `--servers N --clients N --objects N --port P --world minX,maxX,minZ,maxZ --midwares N --autostart [--headless] [--rebalance-alpha F] [--rebalance-threshold F] [--repartition-at TICK --repartition-x "x1,..."]` |
| Midware | `--manager-ip A.B.C.D --manager-port P --server-exe <path> [--headless] [--fixed-step] [--seed N] [--workload shuttle] [--metrics-dir <dir>] [--metrics-capacity N] [--run-seconds N] [--run-ticks N]` |
| Game Server | `--headless`, `--fixed-step`, `--seed N`, `--workload seam\|shuttle\|uniform\|headon`, `--metrics-dir`, `--metrics-capacity`, `--run-seconds`, `--run-ticks`, `--handoff-delay-ticks N`, `--handoff-lookahead N`, `--halo-width W`, `--halo-lookahead N`, `--halo-reliable`, `--physics-threads N`, `--rebalance-interval N`, `--drain-seconds N` — **not passed directly**, see below |
| Client | `--manager-ip A.B.C.D --manager-port P [--game-instance N] [--render-deferred] [--headless] [--run-seconds N] [--interest-radius R]` plus interaction drivers: `[--impulse-test N] [--misroute-every N] [--blast-every N] [--blast-radius N] [--blast-offset-x N] [--spawn-every N] [--destroy-every N] [--drive-every N]` |

> **Game servers are spawned by the midware, not the launcher.** Their launch string is built in `ServerMidwareManager::StartPhysicsServerInstance`, so a flag the game server understands is unreachable unless the midware forwards it. `--fixed-step` and `--seed` are therefore given to the **midware**, which appends them to every server it spawns (`mServerExtraArgs`). Any new game-server flag needs adding in both `ServerStarter.cpp` (to parse it) and `PhysicsServerMidware/ProgramStart.cpp` (to forward it) — otherwise it is silently ignored with no error.
>
> `--fixed-step` pins the physics substep rate (otherwise `mRealHZ`/`mRealDT` adapt to measured frame cost, so servers under different load integrate with different `dt`). `--seed` drives deterministic world construction. **Both are required for any measurement run whose numbers are meant to be comparable.**
>
> **Halo flags.** `--halo-width W` turns cross-border collision on: a server publishes the objects
> within `W` units of a neighbour's region to that neighbour, which holds them as read-only shadows.
> 0 (the default) disables it entirely. `W` has a **hard floor** of
> `v_max * halo_lookahead * dt + 2 * r_max`; the server warns loudly below it rather than silently
> missing contacts. `--halo-lookahead N` (default 4) is deliberately **separate from and much smaller
> than** `--handoff-lookahead`: a handoff gap is one-off, but halo lag is permanent, so shadows are
> extrapolated from their sample tick rather than applied stale. `--halo-reliable` makes the updates
> reliable, which a deployment does not need but a **reproducible run does** - dropped updates are
> not the same from run to run.
>
> The halo also needs both servers to keep pace. Under `--workload shuttle` (359 objects against 41)
> the light server races ahead and `haloLate` reaches essentially every update, so halo runs are
> reproducible under `uniform` and `headon` but **not** under `shuttle`. That makes load balancing a
> correctness prerequisite, not just a performance one.
>
> `--workload shuttle` gives objects an initial X velocity so they cross borders; without it a default world spawns everything inside one region and produces zero handoffs. **It is deliberately adversarial** — every object starts in one region, so a static partition begins ~90% loaded on one server. `--workload uniform` is the balanced counterpart: same motion, but the grid is spread across the whole world (sized from `GetWorldExtent`). Report both — they bracket the contribution at 6.0x and 1.3x busiest-server speedup on 4 servers. `--workload cluster` packs objects into one part of the world and keeps them there - unbalanced but STATIONARY, which is the only way to judge a load balancer: `uniform` is already balanced and `shuttle`'s load moves as fast as a border can. `--workload headon` launches pairs of objects at each other across `x = 0`, one pair per lane, so every collision the run records is a border collision - it is the acceptance test for cross-border collision, since on one server nothing crosses the border and without a halo every object does. `--workload seam` instead centres each grid on the world origin, putting a whole row and column of objects **exactly** on `x = 0` / `z = 0` — the case the half-open ownership rule exists for, and the only way a border check is not vacuous.
>
> `--handoff-delay-ticks N` is **fault injection**: it holds each transfer packet back N ticks while releasing the object locally at the normal moment, deliberately widening the §0.7 ownership gap. Objects still in flight when a run ends are lost, and I2/I5 miss by exactly that count — which is how the gap gets *measured* rather than argued. **Must be 0 for any measurement run.**

**Two run modes, and the choice is methodological.** `--run-seconds N` bounds by wall clock and feeds the loop measured deltas — genuine behaviour under load, but **not reproducible**: tick counts vary with machine load (28.6k–29.4k over nominally identical 60 s runs), and since the border check runs once per *tick*, handoffs land at different simulated times. `--run-ticks N` with `--fixed-step` pins the loop `dt` to the substep length *and* paces each tick to that much real time, so every server stays on one shared clock. End state and conservation then reproduce exactly; handoff *event* counts still vary by ±1, which would need a global tick barrier to remove. Use `--run-ticks` for correctness/conservation experiments and `--run-seconds` with repeats for performance claims.

> Pacing is not optional in reproducible mode. An unpaced fixed-`dt` run lets a lightly loaded server race ahead and **exit while a busier peer is still handing objects to it**; those objects are lost outright (conservation fell to 389/400). That is the §0.7 ownership gap made visible — the sender deactivates on send, so a handoff to a dead peer is unrecoverable.
>
> Beware the realtime `p50`: the loop spins at ~1 kHz while physics substeps at 120 Hz, so ~7 ticks in 8 do **no** physics work and `p50 = 0.017 ms` is the cost of an empty iteration. Paced mode does one substep per tick and gives a tight unimodal distribution. Both agree on `p95 ≈ 1.58 ms`, which is the real per-substep cost.

**Per-tick metrics.** `--metrics-dir <dir>` makes each game server buffer one `TickSample` per tick and write `<dir>/ticks-server<N>.csv` on exit (`DistributedSystemCommonFiles/MetricSink`). Samples are appended to a pre-reserved vector (`--metrics-capacity`, default 200000) so recording never allocates mid-tick; overflow is dropped and counted, and the drop count is printed at flush. **The CSV is only written on a clean exit**, so pair it with `--run-seconds N`, which makes the headless loop return after N seconds — a force-killed server loses its whole buffer.

> The `@@STAT` telemetry line is a *last-sample* reading at ~2 Hz, not an average: on a 60 s two-server run the server-0 tick cost had `p50=0.017 ms` but `mean=0.384 ms` and `p99=2.18 ms`, so a 2 Hz sample lands anywhere between p50 and p99 by luck. Telemetry is for watching a run live; the CSV is what evaluation numbers come from. Servers also tick at *different rates* (they carry different loads), so aggregating per-tick stats across servers needs time-weighting.

**Run model:** `--headless` swaps the OpenGL `ProfilerRenderer` window for a `GameTimer` loop (`DistributedSystemCommonFiles/HeadlessRunner`). Every role prints a `@@STAT role=... key=val ...` line to stdout ~2 Hz (`TelemetryReporter`); a headless midware spawns its game servers windowless (pipe-redirected, no `CREATE_NEW_CONSOLE`) and forwards their stdout tagged `[server N]`. The launcher parses both into a live dashboard + per-entity log tabs, so a headless run shows **one window**. Clients always run windowed. Untick **Headless** for the per-role profiler windows used in evaluation visuals.

## Running experiments

`measure.ps1` is one run. An experiment is a sweep with repeats:

```powershell
# 1, 2 and 4 servers at 400 objects, 3 repeats each
powershell -ExecutionPolicy Bypass -File tools
un-experiments.ps1 -Name scaling -Sweep servers -Values 1,2,4 -Repeats 3
python toolsnalyse.py runs\exp-scaling      # non-zero exit = an invariant failed
```

- **`-Values` is a comma-separated string, not an array.** `powershell -File` does not parse array
  arguments: `-Values 1,2` would arrive as the single value `12` and silently run a 12-server
  experiment.
- **Repeats are not optional.** Per-server object counts and handoff event counts vary by ±1 at the
  same seed (see the reproducibility section of the interactions spec), so performance figures need
  a spread. `analyse.py` reports the **median across repeats**.
- `analyse.py` reads `@@FINAL` totals and the per-tick CSVs, never `@@STAT`; reports percentiles
  rather than means (tick cost is bimodal); and never pools ticks across servers, which would weight
  whichever server ticked more.
- A run that produces fewer CSVs than servers is reported as **FAILED**, not averaged over.

> **Bootstrap messages must be sent reliably.** `GameStartState` was sent with `SendGlobalPacket`
> (unreliable, ENet flag 0) and is one-shot with no retry, so a server that missed it never built
> its world and sat at `game=0` producing no metrics — losing roughly one recipient in four, which
> made 4-server runs fail every time. Snapshots are correctly unreliable because they are superseded
> 60 times a second; **anything one-shot is not**. Use `SendGlobalReliablePacket` for bootstrap.

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
