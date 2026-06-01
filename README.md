# Distributed Physics Server Simulation

A distributed physics server architecture for real-time multiplayer simulation, built as a Newcastle University Computer Game Engineering MSc dissertation project (CSC8599, 2023/2024) by **S.E. Degirmenci**.

The system simulates a single shared physics world by **splitting it across multiple server processes by spatial region**. Each server owns a rectangular slice of the world and simulates only the objects inside it; as an object moves across a region boundary it is **handed off** to the neighbouring server through a network handshake. A central manager orchestrates the servers, and game clients connect to receive replicated world snapshots.

## Repository context

This repo began as a Newcastle MSc monorepo that also held a team coursework "heist" game (inventory, suspicion, guards, etc.) built on the same engine. **That team-game code and its assets have been removed** — the repo is now focused on the distributed physics system plus the shared engine it reuses:

| Category | What it is | Where |
|---|---|---|
| **(B) Distributed Physics Simulation** | The dissertation work — the subject of these docs. | `DistributedPhysicsManager/`, `PhysicsServerMidware/`, `DistributedGameServer/`, the `Distributed*` files in `CSC8503CoreClasses/`, and `CSC8503/` (now only the thin client `DistributedMultiplayerGameScene.*` + its host `DistributedClientStart.cpp`) |
| **(Shared) Engine** | Foundation reused by the four roles: maths, windowing, physics, collision, networking primitives, OpenGL rendering, nav-mesh. | `NCLCoreClasses/`, most of `CSC8503CoreClasses/`, `OpenGLRendering/`, `Recast/`, `Detour/` |

A single entry point, `EntryPoint/main.cpp`, compiles into **one of four executables** depending on three CMake toggles. Which executable you get (Manager, Midware, Game Server, or the thin Client) is chosen at build time. See the [toggle matrix in SETUP.md](docs/SETUP.md#the-build-mode-toggle).

## Quick start (single machine)

This is the minimal happy path with everything on `127.0.0.1`. See [SETUP.md](docs/SETUP.md) for build detail and [DEPLOY.md](docs/DEPLOY.md) for multi-PC deployment.

The distributed system is several differently-configured builds of the *same* solution. You produce one `EntryPoint.exe` per role by flipping the CMake toggles and rebuilding.

1. **Build the Manager** — set `CMAKE_BUILD_FOR_DISTRIBUTED_MANAGER "true"` (others per the [matrix](docs/SETUP.md#the-build-mode-toggle)), delete `CMakeCache.txt`, regenerate, build `EntryPoint`. Save the exe.
2. **Build the Midware** — set `CMAKE_BUILD_FOR_PHYSICS_MIDWARE "true"`, regenerate, build. Save the exe.
3. **Build the Game Server** — set both manager/midware toggles `"false"` (distributed system still active), regenerate, build. Place it where the midware expects it: `./DistributedPhysicsServer/EntryPoint.exe`.
4. **Run the Manager.** Enter the number of physics servers, max clients, and objects per player at the prompts. It listens on port **1234**.
5. **Run the Midware.** Enter the manager IP (`127.0.0.1`) and port (`1234`). It connects and waits.
6. **Press `S` in the Manager window** to create a game instance. The manager computes per-server borders and tells the midware to spawn the game-server process(es).
7. **Build and run the Client** — set all three toggles `"false"` (`CMAKE_DISTRIBUTED_SYSTEM_ACTIVE "false"`), regenerate, build, run. Its host (`RunDistributedClient` in `CSC8503/DistributedClientStart.cpp`) prompts for the manager IP/port, then connects, gets routed to its physics server, and receives snapshots.

## Documentation

| Doc | Contents |
|---|---|
| [docs/SETUP.md](docs/SETUP.md) | Prerequisites, the build-mode toggle, the role matrix, building each role. |
| [docs/DEPLOY.md](docs/DEPLOY.md) | Ports, the launch string, single-machine and multi-PC bring-up, checklists. |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | The four roles, key classes, and the end-to-end bootstrap sequence. |
| [docs/NETWORKING.md](docs/NETWORKING.md) | ENet layer, packet catalogue, snapshot delta/full replication, threading. |
| [docs/SPATIAL-PARTITIONING.md](docs/SPATIAL-PARTITIONING.md) | Region borders and the object-handoff handshake. |

For an agent-oriented summary of the whole repo, see [CLAUDE.md](CLAUDE.md). The original team-project build notes are in [README.txt](README.txt) (superseded by SETUP.md for the distributed system).

## License

See [LICENSE](LICENSE).
