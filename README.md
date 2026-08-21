# Distributed Physics Server Simulation

## Objective and purpose

**The objective is to run one shared physics world across several server machines instead of
one, and to show — with measurements rather than assertions — how well that works and where it
stops working.**

A normal game server simulates the whole physics world by itself. That puts a hard ceiling on
how many objects can exist at once: when the world gets too busy, one machine cannot finish a
physics step in time and everything slows down for everyone. This project removes that ceiling
by **cutting the world into rectangular regions and giving each region to a different server
process**. Each server simulates only the objects inside its own region. When an object moves
across a boundary, the two servers perform a handshake and ownership passes from one to the
other. Clients connect and receive a continuous picture of the whole world, without needing to
know it is being produced by several machines.

**The purpose of the paper** built on this codebase is to answer three questions honestly:

1. **Does splitting the world actually make the simulation faster?**
   Measured answer: yes for the physics itself, which scales very nearly linearly — halving the
   objects per server roughly halves the simulation cost. See
   [Evaluation](docs/EVALUATION.md).
2. **What does the split cost you?**
   Coordination between servers — handing objects over, sharing a band of objects near
   boundaries, sending snapshots to clients — is work that does not exist on a single server,
   and it grows as you add servers. Measuring that cost separately from the physics is the
   central contribution.
3. **Is the result still correct?**
   A distributed simulation can be fast and wrong. The project defines eight invariants (every
   object owned by exactly one server, no objects lost, no objects duplicated, and so on) and
   checks them automatically on every measurement run. A run that breaks an invariant is
   reported as failed, not averaged in.

The system is also compared against **Aura Projection** (Brown, Ushaw & Morgan, I3D 2019), an
earlier distributed physics system from the same institution, by reproducing its published
benchmark. See [the AP benchmark result](docs/superpowers/results/2026-08-21-AP-injection.md).

This began as a Newcastle University MSc dissertation project (CSC8599) by
**S.E. Degirmenci**, and is being extended into a paper.

---

## Table of contents

| Document | What it covers |
|---|---|
| **[Building](docs/BUILDING.md)** | Prerequisites, how to build the four programs, what the one build switch does. |
| **[Tools](docs/TOOLS.md)** | Every script in `tools/`, what it is for, and exactly how to run it. **Start here if you want to use the system.** |
| **[Workloads](docs/WORKLOADS.md)** | The test scenarios (`uniform`, `headon`, `injection`, …), what each one proves, and which to pick. |
| **[Running experiments](docs/RUNNING-EXPERIMENTS.md)** | The full measurement workflow, from build to analysed results, and the rules that keep numbers trustworthy. |
| [Architecture](docs/ARCHITECTURE.md) | The four programs and how a run starts up. |
| [Networking](docs/NETWORKING.md) | Message types, snapshots, replication. |
| [Spatial partitioning](docs/SPATIAL-PARTITIONING.md) | Region borders and the object handover handshake. |
| [Evaluation](docs/EVALUATION.md) | Every experiment and result, as one argument. Read this before quoting any number. |
| [Deploy](docs/DEPLOY.md) | Ports and multi-machine setup. |

---

## How the system is put together

There are **four programs**. They are all built from the same `EntryPoint/main.cpp`; a build
switch decides which one you get.

| Program | Job | Plain description |
|---|---|---|
| **Manager** | Orchestrator | Decides how the world is cut up, tells everyone else what to do. Listens on port 1234. |
| **Midware** | Process launcher | Runs on each physics machine. Starts game server processes when the manager asks. |
| **Game Server** | Physics | Simulates the objects in one region. Hands objects to neighbours when they leave. |
| **Client** | Viewer | Connects and receives snapshots of the world. Can be windowed or headless. |

They start up in that order: the manager waits for midwares and clients to connect, then on
your signal it computes the region boundaries and tells each midware to launch its game
servers. See [Architecture](docs/ARCHITECTURE.md) for the full sequence.

```
        ┌─────────┐
        │ Manager │  port 1234 — decides the regions
        └────┬────┘
             │ "launch a server for region 0"
        ┌────▼────┐
        │ Midware │  one per physics machine
        └────┬────┘
             │ spawns
     ┌───────▼────────┐        ┌────────────────┐
     │ Game Server 0  │◄──────►│ Game Server 1  │   hand objects over
     │ region: x < 0  │        │ region: x >= 0 │   at the boundary
     └───────┬────────┘        └───────┬────────┘
             │ snapshots               │
             └──────────┬──────────────┘
                   ┌────▼────┐
                   │ Client  │  sees one continuous world
                   └─────────┘
```

---

## Quick start

You need **Windows**, **Visual Studio 2022** with C++ tools, **CMake**, and **.NET** (for the
launcher UI). Full detail in [Building](docs/BUILDING.md).

```powershell
# 1. Build everything and copy it into deploy/ (takes a few minutes)
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1

# 2. Start the launcher UI, which runs all four programs for you
dotnet run --project tools\DistributedLauncher\DistributedLauncher.csproj
```

That gives you a running distributed simulation on one machine. To instead take a
**measurement** — a repeatable run that writes numbers to disk — use:

```powershell
# 2 servers, 400 objects, 7200 physics steps, results written under runs\
powershell -ExecutionPolicy Bypass -File tools\measure.ps1 `
    -Servers 2 -Objects 400 -Ticks 7200 -Workload uniform -Tag my-first-run
```

Then read the results:

```powershell
python tools\analyse.py runs\my-first-run
```

See [Tools](docs/TOOLS.md) for what every option means.

---

## Repository layout

| Folder | Contents |
|---|---|
| `NCLCoreClasses/` | Engine foundation: maths, windowing, timers, file loading. |
| `CSC8503CoreClasses/` | Game objects, physics, collision, networking. The `Distributed*` files are the project's own work. |
| `DistributedPhysicsManager/` | The Manager program. |
| `PhysicsServerMidware/` | The Midware program. |
| `DistributedGameServer/` | The Game Server program — where the interesting logic lives. |
| `CSC8503/` | The thin Client, plus the renderer used by the demo build. |
| `OpenGLRendering/` | Renderer backend. |
| `Recast/`, `Detour/` | Vendored navigation-mesh library (unused by the distributed roles). |
| `EntryPoint/` | The shared `main.cpp` and per-role CMake files. |
| `tools/` | Build, run, measure and analysis scripts. See [Tools](docs/TOOLS.md). |
| `docs/` | This documentation. |
| `runs/` | Measurement output. Not committed to git. |

The two files worth reading first, if you want to understand the actual distributed logic, are
`DistributedGameServer/ServerWorldManager.cpp` (region checks and object handover) and
`DistributedPhysicsManager/SystemManager.cpp` (region calculation and run lifecycle).

---

## Automated tests

There is one automated test suite, plus tests for the analysis script:

```powershell
# C++ logic and physics tests
msbuild DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64
.\tools\InteractionTests\Release\InteractionTests.exe    # non-zero exit means failures

# Python tests for the analysis script
python -m unittest discover -s tools -p "test_analyse.py"
```

Most validation, though, is **empirical** — you run the system and read the measurements. That
is what [Running experiments](docs/RUNNING-EXPERIMENTS.md) is for.

---

## A note on honesty in these documents

The documentation deliberately records what does **not** work alongside what does: defects
found, measurements that were retracted and re-taken, and an explicit list of what the evidence
cannot show. This is intentional. A performance claim without its caveats is not a result, and a
distributed system that has never had its correctness questioned has simply not been examined
closely enough.

---

## License

See [LICENSE](LICENSE) if present, or contact the author. The vendored `Recast/`, `Detour/` and
`DebugUtils/` libraries carry their own licenses.
