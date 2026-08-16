# Automated Evaluation Pipeline — Design

*Status: design, not yet implemented. Target: the 6-experiment matrix in `PHDFinder/20-PAPER-PLAN.md`
(MMVE @ MMSys 2027 main target; MMSys OSS & Datasets track in parallel).*

---

## 0. Verdict up front

The paper plan says *"Altyapı hazır"* — the infrastructure (headless mode, `@@STAT`, the WPF
launcher) is enough. **It is not.** The launcher solves *bring-up*, not *measurement*. Four
findings block the evaluation outright, and none of them are fixable in Python:

| # | Blocker | Evidence |
|---|---|---|
| **B1** | **There is no workload.** Objects spawn in a static grid, fall onto the floor, and stop. The *only* thing that ever moves a `TestObject` is a WASD keypress arriving from a human at a game client (`TestObject::Update` → `mPlayerInputs`, set only by `ClientPlayerInputPacket`). With no motion there are **zero border crossings**, so experiment 4 has nothing to measure and experiments 1/3 measure an idle server. | `CSC8503CoreClasses/TestObject.cpp:23-42, 46-52`; `DistributedGameServer/ServerWorldManager.cpp:222-264` |
| **B2** | **The physics timestep is adaptive and self-degrading.** `PhysicsSystem::Update` halves `realHZ` whenever a step overruns and doubles it when there is headroom. Under load the server *silently simulates less* instead of taking longer. A flat "tick time vs. object count" curve would therefore be an artefact, not a result — and `realHZ` is not reported anywhere. | `CSC8503CoreClasses/PhysicsSystem.cpp:80-81, 120-140` |
| **B3** | **Object count is hard-capped at 100/player.** `CreatePlayerObjects` calls `CreateObjectGrid(10, 10, objectsPerPlayer, …)` — a 10×10 grid. `--objects 1000` yields 100. Experiment 3 ("100 → 1000+ per server") cannot run. | `DistributedGameServer/ServerWorldManager.cpp:126-141, 222-264` |
| **B4** | **Roles never terminate and never report.** Every role's loop is `while (true)`; the only teardown is `taskkill /T /F`. There is no `--duration`, no exit code, no flush-on-exit. A run cannot end itself, so it cannot be scripted, and a crashed role is indistinguishable from a finished one. | `CSC8503CoreClasses/DistributedSystemCommonFiles/HeadlessRunner.cpp:13-25`; `tools/DistributedLauncher/RoleProcess.cs:63-72` |

Plus three that corrupt long runs:

- **Snapshot packets are leaked.** `NetworkObject::WriteFullPacket`/`WriteDeltaPacket` `new` a packet;
  `BroadcastSnapshot` sends it and never `delete`s it. At 60 Hz × N objects that is 60 N allocations
  leaked per second (`DistributedGameServer/DistributedGameServerManager.cpp:245-284`).
- **`stateHistory` grows without bound on the server.** Every `WriteFullPacket` does
  `stateHistory.emplace_back(...)`; `UpdateStateHistory` is only ever called on the *client* read path
  (`CSC8503CoreClasses/NetworkObject.cpp:515-545`).
- **`build-deploy.ps1` defaults to `Debug`.** MSVC Debug means no inlining and checked iterators.
  Publishing Debug timings would be indefensible (`tools/build-deploy.ps1:33-34`).

Everything else in this document assumes those are fixed. §7 orders the work so that
**experiment 2 (the single-server baseline) runs end-to-end first**.

---

## A. Metric gap analysis

### A.1 Exact `@@STAT` keys emitted today

There is exactly one emitter, `TelemetryReporter::MaybeEmit`
(`CSC8503CoreClasses/DistributedSystemCommonFiles/TelemetryReporter.cpp:25-67`), rate-limited to one
line per **500 ms** (`kInterval`), called from four sites:

| Call site | Role |
|---|---|
| `DistributedPhysicsManager/ProgramStart.cpp:110` | Manager |
| `PhysicsServerMidware/ProgramStart.cpp:48` | Midware |
| `DistributedGameServer/ServerStarter.cpp:102` | Game server |
| `CSC8503/DistributedClientStart.cpp:70` | Client |

The complete key set — this is the whole of the machine-readable telemetry that exists:

| Role | Key | Source | Meaning | Live? |
|---|---|---|---|---|
| manager | `role=manager` | literal | — | ✅ |
| manager | `midwares` | `Profiler::GetConnectedPhysicsServerMiddlewares` | connected midware count | ✅ (`SystemManager.cpp:199`) |
| manager | `clients` | `GetConnectedGameClients` | connected game clients | ✅ (`SystemManager.cpp:129`) |
| manager | `instances` | `GetStartedGameInstances` | game instances created | ✅ (`DistributedPhysicsManagerServer.cpp:89`) |
| midware | `id` | ctor arg | always `0` — `TelemetryRole::Midware` is constructed with the default `id` | ⚠️ constant |
| midware | `manager` | `GetIsConnectedToGameManager` | 0/1 uplink state | ❌ **`SetIsConnectedToGameManager` is never called anywhere. Always `0`.** |
| server | `id` | ctor arg (`serverId`) | server index | ✅ |
| server | `objs` | `GetObjectsOnBorders` | *actually*: count of `mTestObjects` with physics — i.e. objects this server is simulating. Misnamed. | ✅ (`ServerWorldManager.cpp:93`) |
| server | `total` | `GetTotalObjectsInServer` | `mNetworkIdBuffer - 10` — total network objects **created in the world**, not owned by this server. Every server creates every object. | ✅ (`ServerWorldManager.cpp:120`) |
| server | `phys` | `GetPhysicsTime` | ms of the **last** `mPhysics->Update(dt)` call | ✅ (`ServerWorldManager.cpp:107-111`) |
| server | `world` | `GetWorldTime` | ms of the **last** `mGameWorld->UpdateWorld(dt)` | ✅ (`ServerWorldManager.cpp:95-99`) |
| server | `predict` | `GetPhysicsPredictionTime` | ms of the **last** `PredictFuturePositions` | ✅ (`ServerWorldManager.cpp:101-105`) |
| server | `full` | `GetLastFullSnapshotTime` | ms of the **last** full `BroadcastSnapshot(false)` | ✅ (`DistributedGameServerManager.cpp:110-114`) |
| server | `delta` | `GetLastDeltaSnapshotTime` | ms of the **last** delta `BroadcastSnapshot(true)` | ✅ (`DistributedGameServerManager.cpp:118-122`) |
| server | `game` | `gameStarted` arg | 0/1 | ✅ |
| client | `id` | ctor arg | always `0` — constructed with default `id` | ⚠️ constant |
| client | `fps` | `GetFramesPerSecond` | — | ❌ **only set in `CSC8503/WindowsUI.cpp:57` (imgui); never runs headless. Always `0.00`.** |
| client | `net` | `GetNetworkTime` | — | ❌ **`SetNetworkTime` is never called anywhere. Always `0.00`.** |
| client | `game` | `IsGameStarted()` | 0/1 | ✅ |

**Summary: 13 keys, of which 3 are hardwired zero and 2 are hardwired constants.** The seven live
game-server keys are all *last-sample* scalars — not means, not counts, not distributions.

### A.2 Experiment → metric mapping

| Exp | What the plan asks for | Metrics required | Available today |
|---|---|---|---|
| **1** Scaling 1→8+ servers, constant total objects | server count vs. tick time / throughput | per-server tick time distribution; **actual substep rate** (B2); objects owned per server; handoff rate; aggregate throughput (object-steps/s); CPU per role | `phys` (last sample only), `objs`. **No distribution, no substep rate, no throughput.** |
| **2** Single-server baseline | same as 1 at N=1 | same | same |
| **3** Object sweep 100→1000+/server | object count vs. tick time, **bandwidth** | tick distribution; bytes/s and packets/s per server; memory | **No byte counters at all.** Object count capped at 100 (B3). |
| **4** Handoff accuracy over many crossings | position-error CDF/boxplot | per-handoff record: object id, src/dst server, sim tick, sender authoritative pos, sender predicted pos, receiver resume pos, velocity, handoff latency; **and a ground-truth reference** | **Nothing. Zero handoff instrumentation.** `HandleTransitionHandshakeReceived` is an empty function body (`ServerWorldManager.cpp:207-210`). And with no workload (B1) there are no crossings. |
| **5** Snapshot cost full vs delta | packet size / bandwidth comparison | bytes and packet count split by `Full_State`/`Delta_State`; a way to *force* full-only | **No byte counters.** Full:delta ratio is hardwired 1:5 (`mPacketsToSnapshot = 5`, `DistributedGameServerManager.cpp:108-124`) with no switch. |
| **6** (opt.) client-perceived latency CDF | end-to-end sample per snapshot with a defensible clock story | RTT or one-way samples; snapshot age | **Nothing.** No timestamp in any packet, no ack path from client to server. |

### A.3 Missing instrumentation — the gap table

Naming convention below: new C++ files live in
`CSC8503CoreClasses/DistributedSystemCommonFiles/`.

| # | Metric | Emit where (file : class/function) | Notes |
|---|---|---|---|
| M1 | `tick_wall_us`, `phys_us`, `world_us`, `predict_us`, `handoff_check_us` — **per tick**, not per 500 ms | `DistributedGameServer/ServerWorldManager.cpp::Update` (replace the three `Profiler::Set*Time` calls) + `DistributedGameServer/ServerStarter.cpp::tick` for the outer wall time | Already timed with `high_resolution_clock`; only the *sink* is missing. Keep `Profiler::Set*` for the on-screen profiler. |
| M2 | `physics_substeps`, `real_hz` | `CSC8503CoreClasses/PhysicsSystem.cpp::Update` — `iteratorCount` already exists at line 113 but is discarded | **Mandatory** — without it experiments 1 and 3 are uninterpretable (B2). |
| M3 | `owned_objects`, `total_objects`, `border_objects` per tick | `ServerWorldManager::Update` | `objs`/`total` today are semantically wrong (see A.1); redefine cleanly. |
| M4 | `snapshot_bytes`, `snapshot_packets`, split `full`/`delta` | **`CSC8503CoreClasses/GameServer.cpp::SendGlobalPacket(GamePacket&)`** — the single choke point where `packet.GetTotalSize()` is already computed. Add `mOutgoingBytes += packet.GetTotalSize(); mOutgoingPackets++;` keyed by `packet.type`. The unused `mOutgoingDataRate`/`mIncomingDataRate` members (`GameServer.h:37-38`) were clearly meant for this. | Also read ENet's own `netHandle->totalSentData` for a **wire-level** cross-check including UDP/ENet framing — the paper should report both application bytes and wire bytes. |
| M5 | `snapshot_build_us` per broadcast, per mode | `DistributedGameServer/DistributedGameServerManager.cpp::UpdateGameServerManager:105-124` (timing exists, sink missing) | |
| M6 | **Handoff event record** | sender: `CSC8503CoreClasses/NetworkObject.cpp::FinishTransitionToNewServer` + `DistributedGameServerManager::HandleObjectTransitions` (where `StartSimulatingObjectPacket` is constructed, `NetworkObject.cpp:352-376`); receiver: `DistributedGameServer/ServerWorldManager.cpp::StartHandlingObject:165-205` | See §A.4 — this is the design-heaviest item. |
| M7 | `handoff_count`, `handoff_inflight`, `handoff_failed` | `DistributedGameServerManager::HandleObjectTransitions` | Handoff *rate* is the independent variable that makes exp 1/3 interesting. |
| M8 | Client end-to-end latency sample | server: stamp `FullPacket` at write (`NetworkObject.cpp::WriteFullPacket`); client: echo in a new ack (`CSC8503/DistributedMultiplayerGameScene.cpp`); server: close the loop | See §A.6 — the echo design avoids cross-machine clock sync entirely. |
| M9 | `rss_bytes`, `private_bytes`, `cpu_percent` per role | `CSC8503CoreClasses/Profiler.cpp` already calls `GetProcessMemoryInfo` (line 232-238) but **only exposes it as a `std::string` in MB**. Add numeric getters; add `GetProcessTimes()` for CPU. Sample at 2 Hz. | Cheap. Do it in the same pass as the sink. |
| M10 | Per-object position trace (sim-tick indexed) | `ServerWorldManager::Update`, behind `--trace-objects` | Only needed for the ground-truth handoff metric (§A.4 E2) and the divergence figure. High volume — sample every K ticks. |
| M11 | Run identity / provenance on every row | `RunClock` + `ExperimentConfig` (new) | `run_id`, `role`, `role_id`, `sim_tick`, `t_us` on every record. |

### A.4 Experiment 4 in detail — what "handoff position error" can actually mean

This needs a decision, because the obvious definition is not measurable.

**What exists at handoff.** The sender builds `StartSimulatingObjectPacket`
(`NetworkObject.cpp:352-376`) carrying `lastFullState.position` (authoritative), a
`lastFullState.predictedPosition` taken from `Transform::GetPredictedPosition()`, and the full
velocity/force/inertia set. The receiver, in `ServerWorldManager::StartHandlingObject:178-191`, does:

```cpp
transform.SetPosition(lastNetworkState.predictedPosition);   // resume AT the prediction
transform.SetPredictedPosition(lastNetworkState.position);   // (note: the two are swapped)
```

So at the receiver **both the predicted and the authoritative position are simultaneously in scope**.
That is the natural instrumentation point. What is *absent* is any notion of where the object "should"
have been.

The prediction itself is `PhysicsSystem::PredictFutureStateOfObject(obj, 0.1f)` — a **hardcoded
0.1 s horizon** (`PhysicsSystem.cpp:224-228`). That constant must become a flag
(`--predict-horizon`); error-vs-horizon is a free extra figure and a much stronger result than a
single number.

**Two operational definitions. Implement both.**

- **E1 — continuity error (cheap, ships first).**
  At the receiver's first simulated tick, compare the resumed position against the sender's
  authoritative state extrapolated over the *measured* handoff interval:

  `err_e1 = || p_resume − (p_send + v_send · Δt_handoff) ||`

  All three terms exist in `StartHandlingObject`. `Δt_handoff` is a *one-way* interval between two
  processes, so on a single machine take it directly (§A.6 makes this legitimate), and on multi-machine
  runs record it as `NaN` and fall back to E2. This measures *"did the prediction land where the
  object was going"* — exactly what the thesis' single-object figure showed qualitatively, now as a
  distribution over thousands of crossings.

- **E2 — oracle divergence (the publication-grade metric).**
  Run the identical seeded workload twice: once on the **1-server baseline** (experiment 2, no
  handoffs at all → ground truth) and once on the N-server configuration. Both write M10 traces keyed
  by **`sim_tick`, not wall clock**. Then:

  `err_e2(obj, tick) = || p_distributed(obj, tick) − p_baseline(obj, tick) ||`

  and the handoff-error CDF is `err_e2` sampled at the first tick after each handoff, from the M6 log.
  This also yields a second figure the plan doesn't have — *divergence growth over time* — which is
  the strongest possible answer to "does predictive handoff actually preserve continuity".

  **E2 has a hard prerequisite: cross-run determinism.** Two things break it and both must be handled:
  1. The adaptive `realHZ` (B2) — the two runs will take different substep counts. Fix with
     `--fixed-hz` pinning `realDT`, and drive the sim loop at a fixed rate (§A.5).
  2. **Collision ordering.** Objects are partitioned differently across servers, so the narrow-phase
     pair ordering differs and colliding trajectories will diverge chaotically regardless of handoff
     quality — the metric would measure chaos, not handoff error.
     **Therefore the handoff-accuracy workload must be collision-free between test objects** (assign
     test objects to a non-colliding layer, or stagger them on distinct Y bands). Each object's
     trajectory then depends only on gravity + its own driver, so per-object comparison is exact and
     the residual *is* handoff error. State this explicitly in the paper's methodology — it is a
     legitimate, standard isolation, not a dodge.

**Also broken and worth a footnote in the paper:**
`CalculateIncomingObjectOffsetPosition` (`ServerWorldManager.cpp:298-316`) returns a reference to a
function-local `Vector3` (dangling — UB), has its X-axis branches commented out, and **is never
called**. Either delete it or implement it; do not leave it in a paper artefact.

### A.5 Consideration (a) — 2 Hz is far too coarse; how to record per-tick data without perturbing it

`@@STAT` at 2 Hz over a stdout pipe cannot produce a p99. It is also the wrong *transport*: a headless
midware forwards every game-server line, so 8 servers × per-tick lines would funnel through one pipe
and one WPF dispatcher. That would perturb the measurement badly. **Per-tick data must never touch
stdout.**

**Design: `MetricSink` — in-process, pre-allocated, flushed at end of run.**

```
CSC8503CoreClasses/DistributedSystemCommonFiles/MetricSink.h/.cpp
```

- One sink per *stream* (`ticks`, `snapshots`, `handoffs`, `latency`, `resource`, `trace`).
- Each stream is a **POD record struct**, fixed size, no strings, no allocation on the hot path.
  `TickRecord { uint64 sim_tick; uint64 t_us; uint32 phys_us, world_us, predict_us, tick_us;
  uint16 substeps, real_hz; uint32 owned, border; }` = 48 B.
- Backing store is a `std::vector<T>` **`reserve()`d at construction** to
  `ceil(duration_s × expected_rate × 1.2)`. At 1 kHz × 300 s that is 300 k records × 48 B ≈ **14 MB**.
  Trivial. Zero allocation, zero I/O, zero locks during the measured window — the record is a struct
  copy into a pre-sized buffer.
- **Overflow policy: drop and count.** If the vector is full, increment `dropped` and return. Never
  reallocate mid-run (a `realloc` of 14 MB inside a physics tick would show up as a p99.9 spike caused
  by the instrument). `dropped` goes into `run.json`; a non-zero value **fails the run**.
- **Flush once, at the end**, in the shutdown path — write the whole buffer to
  `<metrics-dir>/<role><id>/<stream>.csv` (or `.bin` + a `.schema.json`; CSV is fine at these volumes
  and makes the dataset self-describing for the OSS track).
- For runs longer than the pre-allocation (or for a crash-tolerant mode), the same class supports a
  **ring buffer + background flusher thread**: writer thread only bumps a head index; a detached
  thread memcpy's completed 64 KB chunks to disk. Use this *only* for the long-soak run; the default
  bounded run uses the simpler flush-at-end path because it is provably non-perturbing.

**Sampling strategy per stream:**

| Stream | Rate | Rationale |
|---|---|---|
| `ticks` | **every tick** | This is the distribution the paper needs. It is one 48-byte store. |
| `snapshots` | every broadcast (60 Hz) | Cheap; bytes are already counted. |
| `handoffs` | every event | Rare by construction. |
| `latency` | every echoed snapshot, capped ~200 Hz | |
| `resource` | 2 Hz | `GetProcessMemoryInfo`/`GetProcessTimes` are syscalls — do not put them on the tick path. |
| `trace` | every K-th tick (`--trace-stride`, default 6 → 20 Hz at 120 Hz sim) | Volume control; 1000 objects × 20 Hz × 300 s × 16 B = 96 MB, acceptable, and it is the *dataset* for the OSS track. |

**Also add in-process histograms** (`Histogram.h`, fixed log-spaced buckets, e.g. 1 µs–1 s, 3
significant digits — an HDR-histogram-lite) so that (i) `@@STAT` can be upgraded to carry live
`p50/p95/p99` for the dashboard, and (ii) the long-soak mode has a bounded-memory fallback. Buckets are
mergeable across repetitions, which is exactly what you want when pooling N=5 reps into one CDF.

`@@STAT` stays as-is for **liveness and progress only** — the harness uses it to decide "the run is
healthy, the game has started, warm-up is over". It is not a data source for the paper.

### A.6 Consideration (b) — clock synchronisation

**Precision of what exists.** `GameTimer` and the inline timing in `ServerWorldManager::Update` use
`std::chrono::high_resolution_clock` (= `steady_clock` on MSVC, QPC-backed, ~100 ns resolution,
monotonic). That is *adequate* for per-tick intervals. Two real problems:

1. `GameTimer::timeDelta` is a **`float`** seconds (`GameTimer.h:24`). At a 300 s run, a float second
   count has ~30 µs of representable granularity — fine for a delta, but **never accumulate wall time
   in float**. `RunClock` must keep `uint64` microseconds.
2. `Profiler::Set*Time` stores `float` **milliseconds**. A 40 µs physics step becomes `0.04` and the
   `@@STAT` formatter prints `%.2f` → **`0.04`, and anything under 5 µs prints as `0.00`**. Sub-10 µs
   resolution is destroyed at the format layer. Record integer microseconds in `MetricSink`.

**Cross-process, same machine — one-way latency IS credible, and this matters.**
On Windows, `steady_clock` is QPC-derived and QPC is *system-wide consistent across processes*
(same origin, same source, documented). So for a single-machine deployment — which is how experiments
1–5 should be run anyway, to remove network variance as a confound — a timestamp taken in the sender
and one taken in the receiver are directly comparable to well under a microsecond. Record
`QueryPerformanceFrequency` and a QPC↔UTC anchor pair in `run.json` so the claim is auditable.
This is what makes E1's `Δt_handoff` (§A.4) legitimate on single-machine runs.

**Cross-machine — do not claim one-way latency.** Windows' default w32tm gives tens of milliseconds
of offset, which is the same order as the quantity being measured. Three options, in preference order:

1. **Server-side echo (recommended, clock-free).** Add `uint64 senderTicks` to `FullPacket`
   (stamped in `NetworkObject::WriteFullPacket`). The client echoes the highest `senderTicks` it has
   applied in a small `SnapshotAckPacket`. The **server** computes
   `rtt = now − echoed_senderTicks` entirely in its own clock. No cross-clock comparison ever occurs,
   so the number is exact regardless of topology. Report the **RTT CDF** as the headline for
   experiment 6. This also finally gives the server the client-ack it has a `TODO` for
   (`DistributedGameServerManager.cpp:257-261`) and lets delta replication pick a real baseline.
2. **Halved RTT for a one-way figure**, stated as such: *"one-way latency estimated as RTT/2, which
   assumes path symmetry; on a switched LAN with symmetric links this is a reasonable approximation
   but is an estimate, not a measurement."* Report it as a secondary axis only.
3. **Snapshot age in sim-ticks** — client reports `server_tick_at_render − server_tick_of_applied_state`.
   Clock-free and dimensionless; excellent for showing *staleness/jitter*, useless for absolute latency.
   Cheap; record it alongside.

If a genuine one-way number is ever required, the honest path is PTP (or `chrony` on Linux hosts) with
the measured offset bound reported in the paper — do not attempt it with `w32tm`.

**Sim-time vs wall-time.** Every record carries **both** `sim_tick` (a monotonically increasing
integer the game server owns) and `t_us`. Cross-process *correlation* (e.g. E2's oracle diff) uses
`sim_tick` and is therefore immune to all clock questions; only latency uses `t_us`.

### A.7 Things that are impossible without changing the C++ — explicit list

Everything in this list requires a code change; no amount of harness or Python work substitutes.

| Required change | File | Why |
|---|---|---|
| `--duration N --warmup N` and a **clean exit(0)** with sink flush; graceful stop on a control signal | `DistributedSystemCommonFiles/HeadlessRunner.{h,cpp}` (add a stop predicate + `onShutdown` callback), all four `ProgramStart`/`ServerStarter` tick lambdas | B4 — without it nothing is scriptable and no data is ever written |
| Fixed-rate sim loop (replace the `sleep_for(1ms)` busy-spin) | `HeadlessRunner.cpp:13-25` | The headless loop currently spins at ~1 kHz calling `PhysicsSystem::Update` with sub-ms `dt`, so CPU% is meaningless and headless≠windowed timing |
| `--fixed-hz` / make `realHZ`,`realDT` **members** of `PhysicsSystem` (they are file-scope globals today) and expose `GetRealHZ()`/`GetLastSubstepCount()` | `PhysicsSystem.cpp:80-81, 83-140`, `PhysicsSystem.h` | B2, and E2 determinism |
| Object count decoupled from the 10×10 grid; `--objects` honoured up to 1000+ | `ServerWorldManager::CreatePlayerObjects`, `CreateObjectGrid` | B3 |
| `WorkloadDriver` — autonomous, seeded object motion | new `DistributedSystemCommonFiles/WorkloadDriver.{h,cpp}`, hooked in `ServerWorldManager::Update` | B1 — **nothing else unblocks experiments 1, 3, 4** |
| Explicit `srand`/`std::mt19937` seeding; remove bare `rand()` | `ServerWorldManager.cpp:237` | Reproducibility |
| Byte/packet counters | `GameServer::SendGlobalPacket` (both overloads), `GameServer.h` | Experiment 5 and 3's bandwidth axis |
| `--snapshot-mode full\|delta\|adaptive` | `DistributedGameServerManager::UpdateGameServerManager:105-124` (replace `mPacketsToSnapshot = 5`) | Experiment 5 needs a full-only arm; today the ratio is unconditional |
| `--predict-horizon` (replace hardcoded `0.1f`) | `PhysicsSystem::PredictFuturePositions:226` | Turns experiment 4 into a curve instead of a point |
| Handoff event hooks | `NetworkObject::FinishTransitionToNewServer`, `ServerWorldManager::StartHandlingObject` | Experiment 4 |
| `SnapshotAckPacket` + `senderTicks` in `FullPacket` + a new `BasicNetworkMessages` entry | `NetworkBase.h` enum, `NetworkObject.{h,cpp}`, `DistributedMultiplayerGameScene.cpp`, `DistributedPacketSenderServer.cpp` | Experiment 6 |
| Numeric memory/CPU getters | `Profiler.{h,cpp}` (today only `std::string` MB) | M9 |
| `delete newPacket` after send; prune server-side `stateHistory` | `DistributedGameServerManager::BroadcastSnapshot`, `NetworkObject::WriteFullPacket` | Long runs otherwise drift on memory and allocator pressure |
| `-Config Release` default | `tools/build-deploy.ps1:33-34` | Debug timings are not publishable |

**Two correctness notes that affect the numbers, not just the plumbing:**

- Delta packets encode position deltas as `(char)currentPos.x` — a **truncating cast to one byte per
  axis** (`NetworkObject.cpp:494-501`). Delta replication is therefore lossy at 1 world-unit
  granularity and undefined for |Δ| ≥ 128. Experiment 5's "delta is cheaper" result **must** be
  reported alongside this fidelity cost, or a reviewer will find it. Consider quantising properly
  (16-bit fixed point) and reporting both variants.
- Most control packets set `size = sizeof(ThePacket)` rather than `sizeof(ThePacket) - sizeof(GamePacket)`
  (compare `FullPacket`/`DeltaPacket`, which are correct). Since `GetTotalSize()` returns
  `sizeof(GamePacket) + size`, those packets are sent with an inflated length. It does not affect the
  snapshot path (the one experiment 5 measures) but it will show up in total-bytes accounting.

---

## B. Harness design

### B.1 Build a separate headless CLI runner. Do **not** extend the WPF launcher.

Justification:

- **WPF cannot run unattended.** It needs an interactive desktop session — no SSH, no Task Scheduler
  under a service account, no CI. An overnight 6-experiment matrix is the entire point.
- **No exit codes, no failure semantics.** `MainWindow.OnLaunch` is `async void` and returns nothing.
  A harness must exit non-zero when a role dies, so that a wrapper script can distinguish "cell
  completed" from "cell crashed and produced a short CSV".
- **The bring-up logic is already the wrong shape.** `await Task.Delay(1200)` / `Task.Delay(1000)`
  between manager→midware→clients (`MainWindow.xaml.cs:27-29`) is a race dressed as a constant. A
  harness must wait on *observed readiness* (`@@STAT ... midwares=N`, then `game=1` from every
  server), with a timeout, or runs will silently start mid-bootstrap.
- **But the process-supervision and remote-agent code is good and tested.** `RoleProcess` (verbatim
  stdout forwarding, exit-code reporting, `taskkill /T /F` tree teardown), `TelemetryParser`
  (`[server N]` de-tagging), `AgentProtocol`/`RemoteAgentClient`/`AgentMode` (NDJSON control channel)
  are exactly what the harness needs. Rewriting them in PowerShell or Python would duplicate the one
  part that already works, including the multi-machine path the plan depends on.

**Therefore: extract, don't rewrite.**

```
tools/
  DistributedLauncher.Core/          # NEW class library, net9.0 (no WPF dependency)
    RoleProcess.cs                   # moved verbatim
    TelemetryParser.cs               # moved verbatim
    AgentProtocol.cs  AgentMode.cs  RemoteAgentClient.cs   # moved
    LaunchProfile.cs                 # moved; gains the new experiment flags
  DistributedLauncher/               # WPF GUI — now just references .Core (unchanged behaviour)
  ExperimentRunner/                  # NEW net9.0 console — the harness
```

`ExperimentRunner` is a .NET console app, not PowerShell or Python, because it reuses `.Core`
directly. It is still perfectly scriptable from PowerShell (`dotnet run --project ... -- run plan.yaml`)
and Python (`subprocess`), which is where the analysis layer lives.

### B.2 `ExperimentRunner` architecture

```
tools/ExperimentRunner/
  Program.cs            # verbs: plan validate | run | resume | collect | doctor
  ExperimentPlan.cs     # YAML/JSON model: matrix, sweeps, reps, defaults
  PlanExpander.cs       # cartesian product -> ordered List<Cell>; applies exclusions
  Cell.cs               # one (config, rep) with a deterministic run_id
  CellRunner.cs         # the state machine below
  Readiness.cs          # @@STAT-driven readiness + liveness gates
  HealthMonitor.cs      # crash / stall / drop detection -> RunOutcome
  Topology.cs           # local vs remote-agent placement of midwares
  RunDirectory.cs       # on-disk layout + run.json manifest
  EnvironmentProbe.cs   # CPU model, cores, RAM, OS build, git sha, dirty flag, exe hashes
  Collector.cs          # pull MetricSink files (local copy / agent file transfer)
  Postcheck.cs          # per-cell validation gates (see B.4)
```

**`CellRunner` state machine** (one cell = one full system bring-up):

```
PREPARE   create run dir; write run.json (config + env); clean stale EntryPoint.exe processes
LAUNCH    Manager --autostart --headless --duration ... --run-id ... --metrics-dir ...
          wait for @@STAT role=manager                          [timeout 10 s]
          local Midware (+ remote agents via RemoteAgentClient)
          wait for @@STAT role=manager midwares=<expected>       [timeout 20 s]
          wait for N × @@STAT role=server id=<0..N-1>            [timeout 30 s]
          Clients (headless, count = plan.clients)
          wait for every server to report game=1                 [timeout 30 s]   <-- run t0
WARMUP    hold plan.warmup_s; roles are told the warmup boundary via --warmup so they
          tag records rather than the harness trimming them post hoc
MEASURE   hold plan.duration_s; poll @@STAT at 2 Hz as a liveness heartbeat only
DRAIN     roles hit --duration, flush sinks, exit(0); harness waits up to 15 s for each
TEARDOWN  taskkill /T /F any survivor; RemoteAgentClient.Stop() for each agent
COLLECT   copy <metrics-dir> trees + stdout logs into the run dir
POSTCHECK gates in B.4 -> outcome = ok | failed | invalid
```

Note the **readiness gate replaces the launcher's fixed delays** and is the single biggest
reliability win. Note also that the game only starts when
`mClientCount == mClientMax` in `DistributedPacketSenderServer::AddPeer` — the harness must launch
*exactly* `--clients` clients or every server hangs at `game=0` forever. `Readiness.cs` turns that
into a clean timeout failure instead of a hang.

### B.3 Failure detection — a crashed role must fail the run

Sources of truth, all of them checked:

1. **Process exit before `DRAIN`.** `RoleProcess.Exited` already reports the code
   (`RoleProcess.cs:67-72`). Any exit before the harness has requested shutdown ⇒ `failed`.
   `0xC0000005` etc. are recorded verbatim in `run.json`.
2. **Non-zero exit at `DRAIN`.** Once roles gain `--duration`, a clean run exits `0`.
3. **Telemetry stall.** No `@@STAT` from a role for > 5 s ⇒ hung ⇒ `failed`.
4. **Never-ready.** Any `LAUNCH` timeout ⇒ `failed`.
5. **Sink-level gates** (§B.4) ⇒ `invalid` (ran, but data unusable).
6. **Remote agent drop.** `RemoteAgentClient` connection closed mid-cell ⇒ `failed`.

Outcome is written to `run.json` **and** encoded in the directory name suffix, so a partial CSV can
never be silently swept into the analysis: `analysis/load.py` refuses to load any run whose
`run.json.outcome != "ok"` unless `--include-failed` is passed. Failed cells are **retried up to
`plan.retries` times with a fresh run_id**; a cell that exhausts retries marks the matrix incomplete
and `ExperimentRunner` exits non-zero.

### B.4 Post-run validation gates

A cell is `invalid` if any of:

- `dropped > 0` in any `MetricSink` (buffer overflow — instrument perturbed the measurement).
- `measure_duration_actual` deviates from `plan.duration_s` by > 2 %.
- Observed tick count < 90 % of `duration_s × expected_hz`.
- Any server reports `real_hz != plan.fixed_hz` at any tick (adaptive stepping leaked in).
- Total handoffs == 0 on a multi-server cell (workload failed to reach the borders — the exact
  failure mode B1 causes, so guard against its return).
- `rss_bytes` grew > 50 % over the measure window (leak regression, see §0).
- Any server's owned-object count is 0 for > 1 s (partition collapse).

### B.5 Multi-machine

`Topology.cs` maps midwares to hosts. Local host always runs the manager (its port 1234 is where
everything rendezvous) and optionally a midware. Remote hosts run `deploy/run-agent.bat`
(`DistributedLauncher.exe --agent --port 5099`, already produced by `build-deploy.ps1:118-134`).

Two additions to `AgentProtocol` are needed and they are small:

- `AgentCommand.ExtraArgs` (string) so the controller can pass `--run-id/--metrics-dir/--duration`
  through to the remote midware — today `AgentMode.StartMidware` hardcodes the arg string
  (`AgentMode.cs:112`).
- `collect` command returning the agent's metrics files (base64 NDJSON chunks, or simply have the
  agent write to an agreed UNC path). File pull is the least code.

Also add `EnvironmentProbe` reporting per host, so heterogeneous machines are visible in the data —
a scaling curve across differently-specced boxes is a confound that must be either eliminated or
disclosed. **Recommendation: run experiments 1–5 entirely on one machine** (server processes are
CPU-isolated via affinity, see below) and use multi-machine only as a supplementary result. That
removes network jitter and clock questions from the headline numbers.

**CPU isolation.** With 8 server processes on one box, they contend. `CellRunner` should set
per-process affinity and priority (`Process.ProcessorAffinity`, `PriorityClass.High`) from
`plan.affinity`, pinning each server to distinct physical cores and keeping the harness/launcher off
them. Record the mapping in `run.json`. Without this, experiment 1's curve measures Windows'
scheduler as much as the system.

---

## C. Workload generation

Recall B1: today nothing moves. This section defines the replacement.

### C.1 `WorkloadDriver`

```
CSC8503CoreClasses/DistributedSystemCommonFiles/WorkloadDriver.{h,cpp}
```

Called from `ServerWorldManager::Update`, before `mPhysics->Update`, in place of the current
`TestObject::Update` input polling. Per object it applies a force or sets a velocity; it never
teleports (teleporting would bypass the border logic being measured).

**Determinism.** Per-object `std::mt19937` seeded as
`seed(run_seed, object_network_id)` via a splitmix/hash — **not** a single shared stream. This is the
key property: an object's trajectory depends only on `(run_seed, its own id, sim_tick)`, so it is
**identical regardless of which server owns it, how many servers there are, or in what order objects
are updated**. That is what makes the E2 oracle comparison (§A.4) valid at all. Bare `rand()`
(`ServerWorldManager.cpp:237`) is removed.

All motion is a pure function of `sim_tick`, never wall-clock `dt`, for the same reason.

### C.2 Workload models

| Model | Parameters | Purpose |
|---|---|---|
| `static` | — | Current behaviour. Control arm: isolates baseline physics cost with zero handoffs. |
| `random_waypoint` | `speed`, `dwell_ticks`, `bounds` | Classic mobility model; reviewers recognise it. Each object picks a uniform target in the world bounds, steers toward it, picks another on arrival. Handoff rate emerges from geometry — *uncontrolled*, which is why it is not the primary model for exp 4. |
| `orbit` | `radius`, `angular_speed`, `centre` | **Primary model for experiment 4.** Objects on circular paths centred on a region border cross it at a *known, exactly controllable* rate: `crossings/s = 2 × angular_speed / 2π` per object. Handoff rate becomes a first-class independent variable — you can plot error vs. crossing rate. Also perfectly deterministic and collision-free if radii/Y-bands are staggered. |
| `border_shuttle` | `crossing_rate_hz`, `amplitude` | Even more direct: objects oscillate perpendicular to the nearest border at a prescribed rate. Use to push handoff rate to saturation for exp 1's stress arm. |
| `hotspot` | `n_clusters`, `sigma`, `drift_speed` | Gaussian clusters that drift across the world. Produces *load imbalance* — the realistic failure mode of static spatial partitioning, and the natural bridge to the "dynamic repartitioning" future work the plan names. Excellent discussion-section figure. |
| `brownian` | `impulse_sigma` | Cheap diffusive spreading; good background load for exp 3's object sweep. |

**Collision policy** is a separate, orthogonal flag (`--collisions on|off`), because exp 4's E2 metric
requires it off (§A.4) while exp 1/3 want it on (collision detection is a major part of the cost being
measured). Report both arms.

### C.3 Specification and injection

Workload parameters are **manager-side configuration that flows to servers over the existing
bootstrap path** — do not add a second configuration channel.

- Manager gains `--workload <model> --workload-params k=v,k=v --seed N --objects N` and validates them.
- They ride to the game servers in `StartDistributedGameServerPacket`, which already carries
  `objectsPerPlayer` and per-server borders (`NetworkObject.h`, `StartDistributedGameServerPacket`).
  Add a `char workloadSpec[256]` field — same fixed-array pattern as the existing `borderStr[256]`,
  which is required because these structs go over the wire by raw `memcpy`. **Do not add
  `std::string` fields** — note the existing `std::string ipAddress`/`createdServerIPs[20]` members
  in these packet structs are already a latent wire-format bug (a `std::string` memcpy'd across a
  process boundary carries a dangling heap pointer); do not add more.
- `ServerWorldManager` constructs its `WorkloadDriver` from that spec at instance start.
- The **same seed and spec are recorded into every `run.json`**, so a run is fully described by
  `(git_sha, plan_cell, seed)`.

### C.4 Object placement

Replace the 10×10 grid (B3) with a placement function that takes `objects_total` and the world bounds:

- `grid` — `ceil(sqrt(n))` × `ceil(n / cols)`, spacing derived from bounds so density is constant as
  `n` sweeps (otherwise exp 3 confounds object count with object density and collision rate).
- `uniform_random` — seeded, for the mobility models.
- `border_band` — objects placed within ±`w` of region borders, for maximum handoff pressure.

The current `CreatePlayerObjects` also has a latent bug: `startPos.x` is only assigned for
`i == 0` and `i == 1` (`ServerWorldManager.cpp:126-141`), so a third player's grid lands at the
origin. Replace the whole function.

---

## D. Data schema and analysis

### D.1 Directory layout

```
experiments/
  plans/
    exp1-scaling.yaml
    exp2-baseline.yaml
    exp3-object-sweep.yaml
    exp4-handoff.yaml
    exp5-snapshot.yaml
    exp6-latency.yaml
  results/
    <plan_name>/
      <run_id>/                       # run_id = <plan>-<cellhash>-r<rep>-<utc>
        run.json                      # manifest: config + environment + outcome
        raw/
          manager/       stdout.log resource.csv
          midware0/      stdout.log resource.csv
          server0/       ticks.csv snapshots.csv handoffs.csv resource.csv trace.csv stdout.log
          server1/       ...
          client0/       latency.csv frames.csv resource.csv stdout.log
      _index.parquet                  # all run.json flattened, one row per run
      _ticks.parquet                  # all ticks.csv concatenated + config columns joined
      _handoffs.parquet
      _snapshots.parquet
      _latency.parquet
  figures/
    fig1_scaling.pdf  fig1_scaling.png
    ...
    figure_data/fig1_scaling.csv      # the exact numbers behind each figure
```

Raw CSV is what the C++ writes (self-describing, greppable, good for the dataset track). Parquet is a
derived cache built by `analysis/build.py` — cheap to regenerate, never hand-edited, gitignored.

### D.2 CSV schemas

Every raw file carries only its own columns; `run_id` and all config columns are joined in from
`run.json` at load time (keeps the hot path free of string writes and keeps files small).

**`run.json`** (one per run — this is the join key table):

```json
{
  "run_id": "exp1-scaling-9f2c1a-r03-20260901T221503Z",
  "plan": "exp1-scaling", "cell_hash": "9f2c1a", "rep": 3,
  "outcome": "ok",
  "config": {
    "servers": 4, "clients": 1, "objects_total": 2000, "objects_per_server": 500,
    "world": [-300, 300, -300, 300],
    "workload": "orbit", "workload_params": {"radius": 40, "angular_speed": 0.8},
    "seed": 20260901, "collisions": true,
    "snapshot_mode": "adaptive", "snapshot_hz": 60, "full_every": 6,
    "sim_hz": 120, "fixed_hz": true, "predict_horizon_s": 0.1,
    "warmup_s": 20, "duration_s": 300, "trace_stride": 6
  },
  "env": {
    "git_sha": "edd16f3", "git_dirty": false, "build_config": "Release",
    "exe_sha256": {"Manager": "...", "DistributedPhysicsServer": "..."},
    "hosts": [{"name":"PHYS-01","cpu":"AMD Ryzen 9 7950X","cores":16,"threads":32,
               "ram_gb":64,"os":"Windows 11 Pro 26200","qpc_freq":10000000,
               "affinity":{"server0":[0,1],"server1":[2,3]}}]
  },
  "timing": {"t0_utc": "...", "measure_start_us": 20000123, "measure_end_us": 320000456},
  "counters": {"dropped_records": 0, "role_exit_codes": {"server0": 0}}
}
```

**`ticks.csv`** — one row per server tick (the workhorse):

| column | type | note |
|---|---|---|
| `sim_tick` | u64 | monotonic, server-owned; the cross-process correlation key |
| `t_us` | u64 | µs since role start (QPC) |
| `phase` | u8 | 0 = warmup, 1 = measure — set by the role, not trimmed post hoc |
| `tick_us` | u32 | whole outer tick wall time |
| `phys_us` | u32 | `PhysicsSystem::Update` |
| `world_us` | u32 | `GameWorld::UpdateWorld` |
| `predict_us` | u32 | `PredictFuturePositions` |
| `border_us` | u32 | `CheckPositionOutOfServerBoundaries` |
| `substeps` | u16 | `iteratorCount` — **M2** |
| `real_hz` | u16 | **M2** |
| `owned_objects` | u32 | objects this server simulates |
| `border_objects` | u32 | objects within the border band |

**`snapshots.csv`** — one row per `BroadcastSnapshot`:
`sim_tick, t_us, phase, mode(full|delta), objects_written, packets, app_bytes, wire_bytes_delta, build_us`

**`handoffs.csv`** — one row per handoff, written by **both** ends and joined on `(object_id, handoff_seq)`:
`sim_tick, t_us, phase, role(sender|receiver), object_id, src_server, dst_server, handoff_seq,
pos_auth_{x,y,z}, pos_pred_{x,y,z}, vel_{x,y,z}, pos_resume_{x,y,z}, dt_handoff_us, err_e1, ack_us, status(ok|timeout|duplicate)`

**`latency.csv`** (server-side, from the echo):
`sim_tick, t_us, phase, client_peer, snapshot_tick, rtt_us, snapshot_age_ticks`

**`resource.csv`** (all roles, 2 Hz):
`t_us, phase, rss_bytes, private_bytes, cpu_user_us, cpu_kernel_us, threads, handles`

**`trace.csv`** (opt-in, exp 4 E2):
`sim_tick, object_id, owner_server, x, y, z` — float32 is sufficient and halves the size.

Every one of these is **tidy/long-form**, so analysis is literally
`df.groupby(["servers","objects_per_server"])["phys_us"].quantile([.5,.95,.99])`.

### D.3 Analysis layer

```
analysis/
  pyproject.toml            # pinned: pandas, pyarrow, numpy, matplotlib, scipy
  build.py                  # raw CSV + run.json -> *.parquet (idempotent, cached)
  load.py                   # load_runs(plan, include_failed=False) -> DataFrame
  style.py                  # single matplotlib rcParams: serif, 8pt, vector PDF, colour-blind-safe
  stats.py                  # ci95(), pooled quantiles, bootstrap CIs, Mann-Whitney
  figures/
    fig1_scaling.py  fig2_baseline.py  fig3_objects.py
    fig4_handoff.py  fig5_snapshot.py  fig6_latency.py
  run_all.py                # build.py + every figure + figure_data CSVs
```

Publication conventions, applied uniformly in `style.py`:

- **Repetitions are the error bar.** Compute the per-run statistic first (e.g. p95 of `phys_us` within
  one run), then plot mean ± 95 % CI **across the N reps**. Never pool raw ticks across reps for a
  central-tendency plot — that hides run-to-run variance, which is the variance a reviewer cares about.
- **Distributions get CDFs, never bar charts with error bars.** Experiments 4 and 6 are CDFs
  (`np.sort` + `linspace`), with a boxplot inset for 4 as the plan requests. Pool raw samples across
  reps for the CDF, but overlay per-rep faint lines so run-to-run stability is visible.
- **Log axes** on: object count (exp 3, log-x), latency and handoff error (log-x on the CDF —
  errors span orders of magnitude), bandwidth (log-y if full vs delta differ by >10×).
- **The baseline is a horizontal reference line on every relevant figure**, as the plan requires
  ("Baseline çizgisi tüm grafiklerde") — `fig2` exports its value to `figure_data/baseline.json`
  and every other figure imports it.
- **Exp 1 gets an ideal-scaling reference line** (throughput ∝ N) so the gap *is* the result.
- Every figure writes `figures/figure_data/<name>.csv` containing exactly the plotted points. This is
  a soft requirement for artifact evaluation and it makes the paper's numbers checkable.

Figure-to-experiment map:

| Figure | Experiment | Form |
|---|---|---|
| `fig1_scaling` | 1 + 2 | x = servers (1..8, linear); left y = p50/p95/p99 `phys_us` with CI ribbons; right y = aggregate throughput (object-substeps/s) vs. ideal line; baseline hline |
| `fig2_baseline` | 2 | tick-time CDF at N=1, per object count; the reference all else is measured against |
| `fig3_objects` | 3 | two stacked panels sharing log-x objects/server: (a) p50/p95/p99 tick time, (b) bytes/s and packets/s |
| `fig4_handoff` | 4 | main: CDF of position error (log-x, m), one curve per server count; inset: boxplot by crossing rate; second panel: E2 divergence vs. time since handoff |
| `fig4b_horizon` | 4 (bonus) | error p50/p95 vs. `--predict-horizon` — a curve the thesis did not have |
| `fig5_snapshot` | 5 | grouped bars: bytes/object/s and packets/s for `full` vs `delta` vs `adaptive`, across object counts; annotated with delta's 1-unit quantisation error |
| `fig6_latency` | 6 | RTT CDF per server count; secondary panel: snapshot-age-in-ticks CDF (the clock-free measure) |

---

## E. Reproducibility (MMSys OSS & Datasets track)

The OSS & Datasets track judges the artifact itself. Deliverables:

**One command.**

```powershell
powershell -ExecutionPolicy Bypass -File tools\reproduce.ps1 -Profile full
```

which: checks prerequisites (`doctor`), runs `build-deploy.ps1 -Config Release`, records
`EnvironmentProbe` output, runs every plan in `experiments/plans/`, then `python analysis/run_all.py`.
`-Profile smoke` runs a ~10-minute reduced matrix (1 rep, 30 s cells, servers ∈ {1,2}) so a reviewer
can validate the pipeline before committing a night to it. `-Profile figures-only` regenerates every
figure from the published dataset without running anything — **this is the mode most reviewers will
actually use**, so it must work against a downloaded dataset with zero build.

**Pinned configuration.** Plans are checked in and content-hashed into `cell_hash`. `analysis/`
dependencies pinned by exact version in `pyproject.toml` + a committed lockfile. The vendored ENet /
Recast trees are already in-repo. Pin the MSVC toolset version in `run.json` (`_MSC_FULL_VER`) —
floating-point codegen differences between toolsets will move the E2 divergence numbers.

**Environment metadata, per run, automatically** — already specified in `run.json.env`: CPU model,
physical/logical cores, RAM, OS build, `QueryPerformanceFrequency`, git SHA + dirty flag, build
configuration, SHA-256 of each deployed `EntryPoint.exe`, and the affinity map. A dirty working tree
sets `git_dirty: true` and `ExperimentRunner` **warns loudly and refuses `-Profile full`** unless
`--allow-dirty` — a paper number produced from uncommitted code is unreproducible by construction.

**Dataset publishing.** The raw tree (parquet + `run.json` + the plans + figure_data) goes to Zenodo
with a DOI, referenced from the paper and the README. Rough sizing: `ticks.csv` at 120 Hz × 300 s ×
48 B ≈ 1.7 MB per server-run; the full 6-experiment matrix with 5 reps is single-digit GB before
compression, low hundreds of MB as parquet+zstd. `trace.csv` dominates — publish it for the exp-4
cells only. Ship a `DATASET.md` with the schema tables from §D.2 verbatim and a
`load_example.ipynb` that reproduces `fig1` in ten lines.

**Also required for a credible artifact:** a `docs/EVALUATION.md` describing the methodology
(warm-up rationale, why collisions are disabled for exp 4, the RTT/2 caveat, the delta-quantisation
caveat), and a smoke test in CI that runs `-Profile smoke` on a self-hosted Windows runner. There is
no test suite today; `-Profile smoke` becomes the de facto regression test for the whole system, which
is worth having independently of the paper.

---

## F. Config file format

YAML (round-trips to the JSON the C++ and `LaunchProfile` already speak).

```yaml
# experiments/plans/exp1-scaling.yaml
plan: exp1-scaling
description: "Scaling curve, constant total object count (Experiment 1 + baseline arm)"

defaults:
  deploy: ../../deploy
  build_config: Release
  clients: 1
  world: [-300, 300, -300, 300]
  workload: orbit
  workload_params: { radius: 40.0, angular_speed: 0.8, y_band: 4.0 }
  collisions: true
  sim_hz: 120
  fixed_hz: true                 # pin realHZ; adaptive stepping is a confound (B2)
  snapshot_mode: adaptive        # 1 full : 5 delta, the shipped behaviour
  snapshot_hz: 60
  predict_horizon_s: 0.1
  trace: false
  warmup_s: 20
  duration_s: 300
  repetitions: 5
  retries: 2

matrix:
  servers:       [1, 2, 3, 4, 6, 8]
  objects_total: [2000]          # held CONSTANT -> objects/server falls as servers rises

seeds: [20260901, 20260902, 20260903, 20260904, 20260905]   # one per repetition

topology:
  manager: local
  midwares:
    - host: local
      servers: all               # or an explicit [0,1,2,3]
  # - host: 192.168.1.42:5099    # remote agent (deploy/run-agent.bat)
  affinity:
    strategy: distinct-physical-cores
    reserve_for_harness: [0]

exclude:
  - { servers: 8, objects_total: 2000, reason: "" }   # placeholder; keep the key for provenance

output:
  root: ../results/exp1-scaling
```

```yaml
# experiments/plans/exp4-handoff.yaml  (the shape that differs most)
plan: exp4-handoff
defaults:
  <<: *common
  workload: orbit
  collisions: false              # REQUIRED for the E2 oracle metric (see A.4)
  trace: true
  trace_stride: 6
  duration_s: 180
  repetitions: 5
matrix:
  servers: [2, 4]
  objects_total: [400]
  workload_params:               # crossing rate becomes the independent variable
    - { radius: 20.0, angular_speed: 0.4 }
    - { radius: 20.0, angular_speed: 0.8 }
    - { radius: 20.0, angular_speed: 1.6 }
  predict_horizon_s: [0.05, 0.1, 0.2, 0.4]
oracle:
  baseline_plan: exp2-baseline   # same seeds, servers=1 -> ground truth traces for E2
```

---

## G. Ordered increment plan

Ordered so that **experiment 2 (the baseline) runs end-to-end as early as possible**, and each
increment leaves the system in a working, useful state.

### Increment 0 — harness skeleton, no C++ changes *(≈1 day)*
- Extract `DistributedLauncher.Core`; WPF launcher references it and still works (regression check).
- `ExperimentRunner` with `plan validate` and `run`, `CellRunner` doing LAUNCH → readiness gates →
  fixed hold → `taskkill` teardown, driven only by existing `@@STAT`.
- `RunDirectory` + `run.json` + `EnvironmentProbe`; crash detection via `RoleProcess.Exited`.
- `build-deploy.ps1` gains `-Config Release` as the default.

**Deliverable:** a 1-server run launches, holds 60 s, tears down cleanly, and produces a `run.json`
with environment metadata and a 2 Hz `@@STAT` CSV. Nothing publishable yet — but the plumbing,
readiness gating, and failure semantics are proven before any engine surgery.

### Increment 1 — the measurement substrate *(≈2 days)* → **experiment 2 becomes real**
- `RunClock`, `ExperimentConfig` (`--run-id --metrics-dir --duration --warmup --seed`), `MetricSink`,
  `Histogram`.
- `HeadlessRunner`: fixed-rate loop, duration-bounded, `onShutdown` flush, `exit(0)`.
- `ticks.csv` from `ServerWorldManager::Update` + `ServerStarter::tick`; `resource.csv` from numeric
  `Profiler` getters in all four roles.
- `PhysicsSystem`: `realHZ`/`realDT` become members; expose `GetLastSubstepCount()`/`GetRealHZ()`;
  add `--fixed-hz`.
- `analysis/build.py`, `load.py`, `style.py`, `fig2_baseline.py`.

**Deliverable: experiment 2 runs unattended and produces a publication-quality tick-time CDF with
error bars over 5 repetitions.** This is the earliest point at which the paper has a real figure.

### Increment 2 — workload *(≈2 days)* → **experiments 1 and 3 unlock**
- `WorkloadDriver` with `static`, `orbit`, `random_waypoint`; per-object seeded RNG.
- Object placement rewrite (removes the 10×10 cap, B3, and the `startPos` bug); `--objects` honoured
  to 1000+.
- Workload spec plumbed manager → `StartDistributedGameServerPacket` (`char workloadSpec[256]`) →
  `ServerWorldManager`.
- Fix the snapshot packet leak and prune server-side `stateHistory` — long runs are now long enough
  to matter.
- `fig1_scaling.py`, `fig3_objects.py` (tick-time panel only).

**Deliverable: experiments 1 and 3 produce scaling and object-sweep curves; handoffs actually occur
and `handoff_count` is non-zero.**

### Increment 3 — bandwidth *(≈1 day)* → **experiment 5**
- Byte/packet counters in `GameServer::SendGlobalPacket`, split by packet type; ENet
  `totalSentData` wire-level cross-check.
- `snapshots.csv`; `--snapshot-mode full|delta|adaptive`.
- `fig5_snapshot.py`; bandwidth panel of `fig3`.

### Increment 4 — handoff, first cut *(≈2 days)* → **experiment 4 (E1)**
- `handoffs.csv` at both ends; `handoff_seq`; `--predict-horizon`.
- Implement or delete `HandleTransitionHandshakeReceived` / `CalculateIncomingObjectOffsetPosition`.
- `fig4_handoff.py` (E1 CDF + boxplot), `fig4b_horizon.py`.

**Deliverable: the position-error distribution the plan asks for, over thousands of crossings.**

### Increment 5 — handoff, oracle *(≈2 days)* → **experiment 4 (E2)**
- `trace.csv` + `--trace-stride`; `--collisions off`; `border_band` and `border_shuttle` workloads.
- `analysis` oracle join (baseline traces vs distributed traces on `sim_tick`), divergence figure.
- Determinism validation gate: two runs, same seed, same server count ⇒ bit-identical traces. If this
  gate fails, E2 is not defensible and must be reported as E1 only — **run this check before
  investing in the rest of Increment 5.**

### Increment 6 — latency *(≈1.5 days)* → **experiment 6 (optional)**
- `senderTicks` in `FullPacket`; `SnapshotAckPacket` + enum entry; client echo; server-side RTT.
- `latency.csv`, snapshot-age-in-ticks; `fig6_latency.py`.
- Multi-machine arm via the extended agent protocol.

### Increment 7 — artifact *(≈2 days)*
- `tools/reproduce.ps1` with `full` / `smoke` / `figures-only`; `-Profile smoke` wired into CI.
- `docs/EVALUATION.md` (methodology + caveats); `DATASET.md`; `load_example.ipynb`.
- Zenodo upload, DOI into README and paper.

**Critical path to a submittable evaluation: Increments 0 → 1 → 2 → 3 → 4.** Increments 5 and 6 are
what turn a solid MMVE short paper into a full one; 7 is what wins the OSS & Datasets track.
