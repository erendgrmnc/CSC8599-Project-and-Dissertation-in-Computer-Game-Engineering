# Launcher server logs + client region overlay

Date: 2026-07-08
Status: approved, not yet implemented

## Problem

Two observability gaps in the distributed physics system, plus one correctness bug
found while diagnosing them.

1. **The launcher shows no physics-server logs.** The `Distributed Physics Launcher`
   creates log tabs for Launcher / Manager / Midware / Client N, but never for the
   game servers, even though the midware forwards their stdout. The Live Status
   dashboard likewise has no server rows. Separately, every tab is flooded with
   `@@STAT` telemetry at 2 Hz, burying real diagnostic output.

2. **The client's world view is uninformative.** It draws replica cubes into an empty
   void with no frame of reference. Nothing indicates the spatial partitioning that is
   the entire point of the system — which server owns which region, or which server is
   simulating a given object.

3. **The manager crashes and starts the game prematurely** (found while reading the
   Manager log tab). This must be fixed first, since a dead manager makes the overlay
   untestable.

## Work item 0 — fix the dangling reference (separate, prior commit)

`DistributedPhysicsManager/SystemManager.cpp:223`:

```cpp
std::vector<DistributedPhysicsServerData*>& GetPhysicsServerDataList(int gameInstanceID) const {
    std::vector<DistributedPhysicsServerData*> dataList;   // local
    ...
    return dataList;                                        // reference to destroyed local
}
```

Its sole caller, `CheckIsGameStartable`, binds `auto& serverList` to the dangling
reference and iterates it. Consequences observed in a live run:

- `Starting Game!` prints on the *first* `DistributedPhysicsServerAllClientsAreConnected`
  packet rather than after all servers report ready — the destroyed vector most likely
  iterates zero times, so the "all servers started" check vacuously returns `true`.
- The manager process then dies. `RunHeadlessLoop` is `while (true)` and never returns,
  so `process exited.` in the launcher means a crash or an escaped exception.

**Fix:** return the vector **by value**. The list is tiny (one entry per physics server)
and built fresh on every call anyway, so there is nothing to optimise. Update the
declaration at `SystemManager.h:75` and the `auto&` binding in `CheckIsGameStartable`
to `const auto`.

**Verification:** run 2 servers headless; `Starting Game!` must appear exactly once,
after both servers have sent their ready packet, and the Manager row must stay
`running`.

This ships as its own commit, before any of the work below.

## Work item 1 — launcher: physics-server logs

### Root cause

Double-tagging. `RoleProcess.cs:56-57` wraps every child line it reads:

```csharp
_process.OutputDataReceived += (_, e) => { if (e.Data != null) LogLine?.Invoke($"[{Label}] {e.Data}"); };
```

The midware has *already* tagged forwarded server lines (`ServerMidwareManager.cpp:132`,
`tag = "[server " + id + "] "`). `TelemetryParser.Parse` strips exactly one `[...] `
prefix, so it sees `Tag = "Midware"` and leaves `[server 0] @@STAT role=server ...` as
the line text. `MainWindow.Ingest` therefore never matches its `Tag.StartsWith("server")`
branch: no `Server N` tab, no server dashboard row, no `@@STAT` parse.

The **remote** agent path is unaffected — `RemoteAgentClient.cs:78` forwards `msg.Line`
verbatim. The fix must preserve that.

### Changes

No new files.

**`TelemetryParser.cs`**
- Replace the loose `string? Tag` with `int? ServerId`, matched by a strict
  `^\[server (\d+)\] ` prefix. Any other bracketed text passes through untouched, so a
  stray log line beginning with `[` cannot hijack tab routing.

**`RoleProcess.cs`**
- Emit child stdout/stderr **verbatim**. Keep the `[{Label}] ` prefix only on
  launcher-authored lifecycle lines (`started (pid ...)`, errors, exit notices), which
  are never `[server N]`-tagged.
- Include the exit code in the exit notice: `process exited (code {ExitCode}).` A
  crashing role is currently indistinguishable from a clean shutdown.

**`MainWindow.xaml.cs`**
- `Ingest` routes `ServerId != null` to tab/row `Server <id>`.
- `@@STAT` lines update the dashboard row and then **return before appending to the log
  text**. Telemetry already drives Live Status; echoing it makes the tabs unreadable.

### Result

`Server 0` / `Server 1` tabs carrying real output (border parse, object handoff
messages), one Live Status row per server showing
`objs / total / phys / world / predict / full / delta`, and no tab drowning in telemetry.

### Also, one misleading log line

`SystemManager.cpp:146` prints `mDistributedPhysicsManagerServer->GetIPAddress()` — the
manager's own listen address, i.e. `0.0.0.0` (`INADDR_ANY`) — while the packet sent on
the next line carries `serverData->GetServerIPAddress()`, the physics server's reported
IP. The log prints a value that is never sent. Change it to print the value actually
sent. This is a logging fix, not a connection fix; the clients connect correctly today.

## Work item 2 — plumbing border data to the client

The client currently has no knowledge of server regions. The manager has them:
`GameInstance::GetServerAreaString` produces `"minX/maxX|minZ/maxZ"`.

**`CSC8503CoreClasses/NetworkObject.h` / `.cpp`**
- Append `char borderStr[256]` to `DistributedClientConnectToPhysicsServerPacket`,
  mirroring `RunDistributedPhysicsServerInstancePacket`, which already ships a border
  string exactly this way. POD appended after the existing fields, so the
  memcpy-over-ENet contract is unchanged and `GetTotalSize()` (`sizeof(GamePacket) + size`)
  picks up the extra bytes automatically.

**`DistributedPhysicsManager/SystemManager.cpp:87`**
- `SendDistributedPhysicsServerInfoToClients` gains a border-string parameter, sourced
  from the instance's `GetServerBorderStrMap()`.

**`CSC8503CoreClasses/DistributedSystemCommonFiles/DistributedUtils.h` / `.cpp`**
- Add `bool ParseBorderString(const std::string&, float& minX, float& maxX, float& minZ, float& maxZ)`.
  Returns `false` on malformed input.

The client receives one such packet per server (the manager broadcasts globally) and
accumulates one region per packet.

> Note: the game server's own `CreatePhysicsServerBorders`
> (`DistributedGameServerManager.cpp:349`) parses this same format with `std::stoi`,
> silently truncating fractional bounds. The new helper uses `std::stof`. Fixing the
> server's copy is **out of scope**.

## Work item 3 — client: ownership attribution

Objects are tinted by **which physics server actually sent the last snapshot for them**,
not by which region their position falls in. Geometric containment would merely restate
the partitioning rule; snapshot source shows true ownership, so a cube visibly changes
colour the instant a handoff completes, and a handoff that silently fails stays the
wrong colour.

**`CSC8503/DistributedMultiplayerGameScene.h` / `.cpp`**
- `mDistributedPhysicsClients` becomes a vector of `{GameClient* client; int serverId;}`.
- `UpdatePhysicsClients` sets `mActiveServerId` immediately before each
  `client->UpdateClient()`, and resets it to `-1` after the loop. `ProcessPacket`
  dispatches synchronously inside `UpdateClient`, so `HandleFullPacket` /
  `HandleDeltaPacket` can attribute the packet to `mActiveServerId`. The method loses its
  `const` qualifier.
- Ownership is stamped per network object and used to set the replica's `RenderObject`
  colour.
- New region state: `struct ServerRegion { int serverId; float minX, maxX, minZ, maxZ; Vector4 colour; }`,
  plus `GetServerRegions()` and `GetWorldBounds()` accessors. `GetWorldBounds()` returns the
  axis-aligned union of all regions received so far — the client is never told the world
  bounds directly, and reconstructing them from the union is exact because the manager
  tiles the world without gaps or overlap (`GameInstance::CalculateServerBorders`).

This is safe against reentrancy: `ConnectClientToDistributedGameServer` is invoked from
inside the *manager* client's `UpdateClient`, which runs in `UpdateDistributedManagerClient`
before `UpdatePhysicsClients` — so the physics-client vector is never mutated while being
iterated.

### Palette

Fixed 8-entry table indexed `serverId % 8`: cyan, orange, green, magenta, yellow, red,
violet, teal. A region's outline and its objects share one colour. Unknown server id
falls back to the current default blue.

## Work item 4 — client: the overlay

### What is drawn

- A ground-plane wireframe rectangle per server region at `y = 0`, in that server's colour.
- Every replica cube tinted to its owning server's colour.
- A top-left text legend mapping colour → `Server N`, plus the current toggle state.

### Components

**`CSC8503/DistributedClientOverlay.h` / `.cpp`** (new; added to `CSC8503/CMakePC.cmake`,
wrapped in `#ifndef DISTRIBUTEDSYSTEMACTIVE`)
- Pure emitter: takes the region list and issues `Debug::DrawLine` for each region
  rectangle and `Debug::Print` for the legend. Holds no GL state and no scene knowledge.
  Keeps the scene free of rendering concerns and `DistributedClientStart.cpp` thin.

**`CSC8503/DistributedClientRenderer.h` / `.cpp`**
- Gains the debug-line and debug-text passes lifted from `GameTechRenderer.cpp:773-880`,
  plus a `LoadDebugTexture` equivalent for the font atlas.
- These are plain VAO + `DebugLines.*` / `DebugText.*` shaders with **no bindless texture
  handles**, so they do not reintroduce the GPU-hang failure mode that this "safe"
  forward renderer exists to avoid. That hazard lives in the deferred path only.

Required assets all exist: `Assets/Shaders/DebugLines.{vert,frag}`,
`Assets/Shaders/DebugText.{vert,frag}`, `Assets/Fonts/PressStart2P.fnt`,
`Assets/Textures/PressStart2P.png`.

**`CSC8503/DistributedClientStart.cpp`**
- `Debug::CreateDebugFont` at startup; `Debug::UpdateRenderables(dt)` each frame.
- `KeyCodes::F3` (`Keyboard.h:116`) toggles the visualisation.
- Camera framing (below).

### Toggle semantics

`F3` toggles the whole visualisation as one unit: grid + legend + object tint. Off is the
current appearance — plain blue cubes, no grid, no legend. The replica's base colour is
retained so the tint can be reverted. The new state is also written to `std::cout`, so it
appears in the launcher's Client tab.

The overlay starts **on**. It is the point of the feature, and a client that launches
looking exactly as it does today would suggest the build had not picked up the change.

### Camera framing

The client currently starts at `(0, 220, 260)` looking at an empty 300x300 world, so a
single scale-4 cube is an unreadable speck and most of the grid would be off-screen. Once
the first region arrives, the host frames the camera to the aggregate world bounds. Done
once, on the first region packet; the user is free to move afterwards.

## Error handling

| Condition | Behaviour |
|---|---|
| Malformed / empty `borderStr` | Log and skip that region. The client still connects and receives snapshots; that region is simply not drawn. |
| Snapshot from an unknown server id | Object keeps the default blue tint. |
| Missing font or debug shader assets | Overlay disables itself; the frame loop and cube rendering continue. |
| Launcher: a line beginning with `[` that is not `[server <int>] ` | Passed through as literal text; routed to its source tab. |

## Testing

There is no automated test suite in this repository; validation is empirical, as with the
rest of the project (`docs/DissertationEvaluationVisuals/`). Manual acceptance:

1. **Work item 0 in isolation.** 2 servers, headless. `Starting Game!` appears exactly
   once, after both servers report ready. Manager row stays `running` for the duration.
2. **Logs.** `Server 0` and `Server 1` tabs exist and contain non-telemetry output. Live
   Status has a row per server with physics/world/snapshot timings. No `@@STAT` line
   appears in any log tab. Killing a role shows a non-zero exit code.
3. **Regions.** With 2 servers, the drawn grid matches the manager's `SERVER BORDERS`
   block (`Server(0): -150,0|-150,150`, `Server(1): 0,150|-150,150`). Repeat with 4
   servers and confirm the 2x2 partition.
4. **Ownership.** A cube crossing x=0 changes colour at the line.
5. **Toggle.** `F3` turns grid, legend and tint off and back on.
6. **Regression.** Untick Headless; the per-role profiler windows still open. The
   non-distributed demo build (`DISTRIBUTEDSYSTEMACTIVE` off) still compiles — the new
   overlay is guarded, and the three server roles must not link it.

## Out of scope

- The server's `std::stoi` border parse (`DistributedGameServerManager.cpp:380`).
- The `std::string`-inside-a-memcpy'd-`GamePacket` hazard, which the existing protocol
  already relies on (short strings survive via SSO).
- Any change to the deferred `GameTechRenderer` path.
