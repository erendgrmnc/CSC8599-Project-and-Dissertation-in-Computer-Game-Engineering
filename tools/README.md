# Deployment / configuration tooling

Tools for configuring and launching the distributed physics system without the
per-role console prompts.

## Contents

| Item | What it does |
|---|---|
| `build-deploy.ps1` | Builds all four roles (one per CMake toggle) into `deploy/<Role>/EntryPoint.exe` + DLLs. |
| `DistributedLauncher/` | A .NET WPF launcher that configures a run and spawns the roles. |

## 1. Produce the role executables

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1
```

This regenerates and builds the solution four times (Manager / Midware / Client /
Game Server) and stages the exes under `deploy/`:

```
deploy/
  Manager/EntryPoint.exe
  Midware/EntryPoint.exe
  Client/EntryPoint.exe
  DistributedPhysicsServer/EntryPoint.exe   <- spawned by the midware
```

The original CMake toggle (`true/false/true`) is restored when the script finishes.

## 2. Run the launcher

```powershell
dotnet run --project tools\DistributedLauncher\DistributedLauncher.csproj
```

or open `tools/DistributedLauncher/DistributedLauncher.sln` in Visual Studio 2022.

In the launcher:

1. Confirm the **Deploy folder** (auto-detected if `deploy/` sits above the exe).
2. Set physics servers, clients, objects/player, **world boundaries**, manager IP/port.
3. Leave **Headless** ticked (default) for a clean run, or untick it to get the per-role
   OpenGL profiler windows for evaluation screenshots.
4. Click **Launch System**. The launcher starts the Manager (with `--autostart`),
   then the local Midware (which spawns the game servers), then the Clients.

Profiles can be saved/loaded as JSON.

### Single pane of glass

In headless mode the role processes open **no windows** — the launcher is the only
window. It shows:

- a **Live status** dashboard with one row per entity (Manager, Midware, each Game
  Server, each Client) updated from the roles' telemetry — e.g. connected
  clients/midwares, objects per server, physics/snapshot timings, `game` started;
- a **per-entity log tab** for each role; game-server tabs appear automatically as the
  midware forwards their output.

This works for both single-device runs and remote machines (telemetry is forwarded
from each remote midware through its agent back to the controller).

Untick **Headless** to restore the per-role profiler windows; the dashboard still works.

### Configuration flags

The launcher passes these flags to the role exes (parsed by `LaunchConfig` in
`CSC8503CoreClasses/DistributedSystemCommonFiles`). They also work when running an
exe by hand; with no flags, the roles fall back to the original console prompts.

| Role | Flags |
|---|---|
| Manager | `--servers N --clients N --objects N --port P --world minX,maxX,minZ,maxZ --midwares N --autostart [--headless]` |
| Midware | `--manager-ip A.B.C.D --manager-port P --server-exe <path> [--headless]` |
| Client | `--manager-ip A.B.C.D --manager-port P [--game-instance N] [--headless]` |

`--headless` runs a role windowless (a `GameTimer` loop instead of the OpenGL profiler
window). Every role also prints one `@@STAT role=... key=val ...` telemetry line to
stdout ~twice a second, which the launcher parses into its dashboard. A headless midware
spawns its game servers windowless too and forwards their stdout (tagged `[server N]`).

## 3. Remote midware machines (agent mode)

The same launcher binary runs as a headless agent on a remote machine and spawns the
midware there on command from the controller (see the **Remote midware agents** box
in the GUI). This is Milestone 4.
