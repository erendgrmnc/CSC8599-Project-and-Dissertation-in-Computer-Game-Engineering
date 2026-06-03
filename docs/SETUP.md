# Setup & Build

How to build the Distributed Physics Server Simulation. For running and connecting the processes, see [DEPLOY.md](DEPLOY.md).

## Prerequisites

- **Windows** (the system uses Win32 `CreateProcessA` to spawn servers and Winsock via ENet).
- **Visual Studio 2019/2022** with the C++ desktop workload (MSBuild + the MSVC toolchain).
- **CMake ≥ 3.16** (`CMakeLists.txt:1`). CMake *generates* the `DistributedPhysicsSystem.sln`; you do not edit the solution by hand.
- **C++20** (`CMakeLists.txt:7`).
- **x64** platform. (A PlayStation 5 / `Prospero` path exists in the CMake files but is out of scope here.)

Assets are resolved from a path baked into the binary at configure time via the `ASSETROOTLOCATION` compile definition, pointed at `Assets/` (`CMakeLists.txt:42-47`).

## The build-mode toggle

This is the single most important concept. **There is one entry point, `EntryPoint/main.cpp`, and it compiles into one of four different executables** depending on preprocessor definitions. Those definitions come from three variables near the top of the root `CMakeLists.txt`:

```cmake
# CMakeLists.txt:10-12
set(CMAKE_DISTRIBUTED_SYSTEM_ACTIVE "true")     # off -> builds the standalone team game
set(CMAKE_BUILD_FOR_DISTRIBUTED_MANAGER "false")
set(CMAKE_BUILD_FOR_PHYSICS_MIDWARE "true")
```

They expand to the compile defs `DISTRIBUTEDSYSTEMACTIVE`, `BUILDFORDISTRIBUTEDMANAGER`, and `BUILDFORPHYSICSMIDWARE` (`CMakeLists.txt:18-29`), which `main.cpp` switches on to `#include` the matching `ProgramStart.cpp` and call its entry function (`EntryPoint/main.cpp:1-34`):

```cpp
#ifdef DISTRIBUTEDSYSTEMACTIVE
  #ifdef BUILDFORDISTRIBUTEDMANAGER
    #include "../DistributedPhysicsManager/ProgramStart.cpp"   // Manager
  #elif BUILDFORPHYSICSMIDWARE
    #include "../PhysicsServerMidware/ProgramStart.cpp"        // Midware
  #else
    #include "../DistributedGameServer/ServerStarter.cpp"      // Game Server
  #endif
#else
  #include "../CSC8503/DistributedClientStart.cpp"             // Thin distributed client
#endif
```

### Role matrix

| Role | `..._SYSTEM_ACTIVE` | `..._FOR_DISTRIBUTED_MANAGER` | `..._FOR_PHYSICS_MIDWARE` | Compile defs | Entry function |
|---|---|---|---|---|---|
| **Distributed Manager** | `true` | `true` | `false` | `DISTRIBUTEDSYSTEMACTIVE`, `BUILDFORDISTRIBUTEDMANAGER` | `StartProgram()` |
| **Physics Server Midware** | `true` | `false` | `true` | `DISTRIBUTEDSYSTEMACTIVE`, `BUILDFORPHYSICSMIDWARE` | `StartMidware()` |
| **Distributed Game Server** | `true` | `false` | `false` | `DISTRIBUTEDSYSTEMACTIVE` | `StartGameServer(argc, argv)` |
| **Thin Client** | `false` | (ignored) | (ignored) | *(none of the above)* | `RunDistributedClient()` |

> When `..._DISTRIBUTED_MANAGER` is `true`, the midware toggle is ignored (the manager branch is checked first). When both are `false` but the system is active, you get the Game Server.

## Building a role

The same steps apply to every role — only the toggle values differ.

1. **Edit the toggles** in `CMakeLists.txt:10-12` for the role you want (see the matrix).
2. **Clear the CMake cache** so the changed defs take effect:
   ```powershell
   Remove-Item CMakeCache.txt -ErrorAction SilentlyContinue
   ```
3. **Regenerate** the solution:
   ```powershell
   cmake -G "Visual Studio 17 2022" -A x64 .
   ```
   (Use `"Visual Studio 16 2019"` if that is your installed toolset.)
4. **Build** the `EntryPoint` project — it is already set as the startup project (`CMakeLists.txt:86`). Either open `DistributedPhysicsSystem.sln` and build, or:
   ```powershell
   msbuild DistributedPhysicsSystem.sln /t:EntryPoint /p:Configuration=Debug /p:Platform=x64
   ```

**Only one role is produced per build.** To run the full distributed system you must repeat the steps above for each role, saving each resulting `EntryPoint.exe` to a labelled location before rebuilding the next. A practical convention:

```
build-out/
  Manager/EntryPoint.exe
  Midware/EntryPoint.exe
  DistributedPhysicsServer/EntryPoint.exe
```

### Game Server placement (important)

The midware launches the game server by an exact relative path: **`./DistributedPhysicsServer/EntryPoint.exe`** (`PhysicsServerMidware/ServerMidwareManager.cpp:9`, used at `:93`/`:115`). The Game Server build must be copied to that path *relative to the midware's working directory*, or the spawn will fail with a `CreateProcess failed` message (`ServerMidwareManager.cpp:127`).

## Thin client (non-distributed build)

Set `CMAKE_DISTRIBUTED_SYSTEM_ACTIVE "false"` and rebuild to get the **thin distributed client** (`RunDistributedClient()` via `CSC8503/DistributedClientStart.cpp`). This is the player-facing process: it opens a window + profiler, prompts for the manager IP/port, connects, and receives world snapshots. (Historically this toggle built the now-removed CSC8503 heist game; the branch was repurposed for the client when the team-game code was deleted.)

> The Vulkan renderer and the PS5/Prospero path were removed during cleanup. All four roles build against the OpenGL renderer only.
>
> The generic engine systems that the team game once drove — level/room loading (`Level`, `Room`, `JsonParser`), `AnimationSystem`, FMOD `SoundObject`, `GameTechRenderer` + imgui UI, and the `RecastBuilder` nav-mesh generator — were later **restored, decoupled from the deleted gameplay**, for future demo-assessment use. The renderer/animation/sound pieces are guarded with `#ifndef DISTRIBUTEDSYSTEMACTIVE`, so they compile only into this non-distributed build and stay out of the lean server roles.

## Headless run mode + the launcher (recommended)

Bringing the roles up by hand opens a window per role plus a console per game server,
which is hard to track. Instead, use the **GUI launcher** in [`tools/`](../tools/README.md):
it spawns every role **headless** (each role accepts `--headless` to run a `GameTimer`
loop with no OpenGL window) and shows a single window with a **live status dashboard** and
**per-role log tabs**. Every role prints a `@@STAT role=... key=val ...` telemetry line to
stdout that the launcher parses; a headless midware spawns its game servers windowless and
forwards their output. This is the same flow for single-device and multi-device runs.

Untick **Headless** in the launcher to restore the per-role profiler windows for
`docs/DissertationEvaluationVisuals` screenshots.

## Next

- [DEPLOY.md](DEPLOY.md) — ports, the launch string, and how to bring the processes up in order.
- [ARCHITECTURE.md](ARCHITECTURE.md) — what each role does once it is running.
