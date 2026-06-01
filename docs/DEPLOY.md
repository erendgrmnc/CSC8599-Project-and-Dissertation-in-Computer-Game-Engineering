# Deployment & Running

How to bring the processes up and connect them. Assumes you have already produced one `EntryPoint.exe` per role — see [SETUP.md](SETUP.md).

## Topology

```
                       +-----------------------------+
                       |   Distributed Manager        |
                       |   (orchestrator, port 1234)  |
                       +--------------+--------------+
                          ^           |            ^
           connects (1234)|           | spawn cmd  | connects (1234)
                          |           v            |
          +---------------+---+   +---+------------+----+
          | Physics Midware    |  | Physics Midware ... |
          | (CreateProcessA)   |  |                     |
          +---------+----------+  +---------------------+
                    | launches
                    v
          +------------------------------+
          | Distributed Game Server      |   <-- simulates one spatial region
          | packet-sender port = peer*10 |
          |                       +1000   |
          +---------------+--------------+
                          ^
            snapshots     | (Full_State / Delta_State)
                          |
                    +-----+------+
                    | Game Client |  first connects to Manager (1234),
                    +-------------+  then is routed to its physics server
```

The **Manager** is the hub. **Midwares** connect to it as clients and act as process launchers. Each **Game Server** opens its own *packet-sender* port that clients connect to for world snapshots. A **Client** connects to the manager first, is told which physics server/port to use, then connects there.

## Ports

| Port | Owner | Source |
|---|---|---|
| **1234** | Manager listen port (fixed). Midwares, game servers, and clients all dial it. | `DistributedPhysicsManager/ProgramStart.cpp:24` (`SYSTEM_MANAGER_PORT`) |
| **`(peerID * 10) + 1000`** | Each Game Server's packet-sender port that clients connect to. peerID is assigned by ENet on connection, so e.g. peer 1 → 1010, peer 2 → 1020. | `DistributedGameServer/DistributedGameServerManager.cpp:64` |

Because the game-server port is derived from the ENet peer ID, two game servers on the **same machine** get distinct ports automatically.

## The game-server launch string

The midware does not embed game-server config in a packet payload it parses itself — it forwards it as a **command line**. It builds the argument string (`ServerMidwareManager.cpp:89`):

```
--arg1 <ip>-<managerPort>-<serverID>-<gameInstanceID>-<borders>
```

and spawns `./DistributedPhysicsServer/EntryPoint.exe` with it via `CreateProcessA` (`ServerMidwareManager.cpp:92-128`). The Game Server reads `argv[2]` (the part after `--arg1`) and splits it on `-` (`DistributedGameServer/ServerStarter.cpp:66-91`):

| Field | Example | Meaning |
|---|---|---|
| ip | `127.0.0.1` | Manager IP to connect back to |
| managerPort | `1234` | Manager port |
| serverID | `0` | This server's logical ID |
| gameInstanceID | `1` | Which game instance it belongs to |
| borders | `-150/150\|-150/150` | This server's region, encoded `minX/maxX\|minZ/maxZ` |

See [SPATIAL-PARTITIONING.md](SPATIAL-PARTITIONING.md) for how borders are computed and encoded.

## Console prompts per role

- **Manager** (`DistributedPhysicsManager/ProgramStart.cpp:47-57`): *"Enter Physics servers to start"*, *"Enter max clients to connect"*, *"Enter objects to create per player"*. Then it listens on 1234.
- **Midware** (`PhysicsServerMidware/ProgramStart.cpp:10-26`): *"Please enter distributed manager ip address"* (default `127.0.0.1`; type `e` to keep the default), then *"Please enter distributed manager port"*.
- **Game Server**: no prompts — it is launched by the midware with the argument string above and opens a profiler window.

### Manager hotkeys (`ProgramStart.cpp:68-87`)

| Key | Action |
|---|---|
| `S` | Create a new game instance (computes borders, triggers server spawn when startable). |
| `SPACE` | Send a test broadcast string packet. |
| `T` | Move the window to (0,0). |
| `PRIOR` / `NEXT` (PageUp/PageDown) | Show / hide the console. |

## Single-machine bring-up (localhost)

1. Start **Manager**; answer the three prompts (e.g. `1` server, `1` client, `1` object). Confirm it prints `Starting server on port: 1234`.
2. Start **Midware**; enter `127.0.0.1` and `1234`. It connects and is assigned a midware ID.
3. In the Manager window press **`S`**. The manager computes borders, sends a run-instance packet to the midware, and the midware spawns `./DistributedPhysicsServer/EntryPoint.exe` in a new console.
4. Start a **Client** build; it connects to `127.0.0.1:1234`, receives its physics-server address/port, connects there, and begins receiving snapshots once the game starts.

## Multi-PC bring-up

| Machine | Role | Notes |
|---|---|---|
| PC-A | Manager | Note its LAN IP (e.g. `192.168.1.10`). |
| PC-B, PC-C, … | Midware (one per physics machine) | Enter PC-A's IP and `1234` at the prompt. Each must have `./DistributedPhysicsServer/EntryPoint.exe` present so it can spawn game servers locally. |
| any | Clients | Point them at PC-A's IP / `1234`. |

Order: **Manager → Midware(s) → press `S` on Manager → Clients.** The game servers are spawned by the midwares; you never launch them by hand in normal operation (though you can, using the launch string above, for debugging).

### Firewall

Open the manager port **1234** on the manager machine, and the game-server **packet-sender ports** (`1000 + peerID*10`, i.e. the low `10xx`/`10x0` range) on each machine that runs game servers. All transport is ENet over UDP.

## Bring-up checklist

- [ ] One `EntryPoint.exe` built per role (Manager, Midware, Game Server) — see [SETUP.md](SETUP.md).
- [ ] Game Server exe present at `./DistributedPhysicsServer/EntryPoint.exe` relative to each midware.
- [ ] Manager running, prints `Starting server on port: 1234`.
- [ ] Each midware connected (manager logs the midware connection; midware prints its assigned ID).
- [ ] Pressed `S` on the manager; game-server console window(s) appeared.
- [ ] Client(s) connected and receiving `Full_State` / `Delta_State` snapshots.
- [ ] Firewall allows 1234 + the `10xx` packet-sender ports (multi-PC).

## Next

- [ARCHITECTURE.md](ARCHITECTURE.md) — the full bootstrap sequence behind these steps.
- [NETWORKING.md](NETWORKING.md) — what travels over each connection.
