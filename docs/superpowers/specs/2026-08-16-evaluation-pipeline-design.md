# Automated Evaluation Pipeline — Design (v2)

*Status: design only. No implementation.*
*Supersedes `2026-08-15-evaluation-pipeline-design.md`, which remains valid where not contradicted;
this revision folds in a second audit pass that found six **measurement-validity** defects the first
pass did not, and re-derives the increment order around them.*
*Target: the 6-experiment matrix in `PHDFinder/20-PAPER-PLAN.md` (MMVE @ MMSys 2027 primary; MMSys
OSS & Datasets track in parallel; arXiv Nov 2026).*

---

## 0. Verdict up front

The paper plan states *"Altyapı hazır"* — headless mode, `@@STAT`, and the WPF launcher suffice.
They do not. The launcher solves **bring-up**; it does not solve **measurement**. And measurement is
not merely absent: for four of the six experiments the system as it stands would produce numbers that
are *wrong in a direction a reviewer can detect*, which is worse than producing none.

Two classes of blocker. Both must be cleared in C++; neither is fixable in Python or in the harness.

### 0.1 Class I — infrastructure blockers (from the v1 audit; unchanged and still binding)

| # | Blocker | Evidence |
|---|---|---|
| **B1** | **There is no workload.** `TestObject` only moves on WASD input arriving from a human client. Objects spawn in a grid, fall, and stop. Zero border crossings ⇒ experiment 4 has nothing to measure and 1/3 measure an idle server. | `CSC8503CoreClasses/TestObject.cpp:23-42, 46-52`; `DistributedGameServer/ServerWorldManager.cpp:222-264` |
| **B2** | **Object count hard-capped at 100/player** by `CreateObjectGrid(10, 10, …)`. `--objects 1000` yields 100. Experiment 3's stated range (100 → 1000+) cannot be run. | `DistributedGameServer/ServerWorldManager.cpp:126-141, 222-264` |
| **B3** | **Roles never terminate and never report.** Every loop is `while (true)`; teardown is `taskkill /T /F`. No `--duration`, no exit code, no flush-on-exit. A run cannot end itself, so it cannot be scripted, and a crash is indistinguishable from a finish. | `CSC8503CoreClasses/DistributedSystemCommonFiles/HeadlessRunner.{h,cpp}`; `tools/DistributedLauncher/RoleProcess.cs:63-72` |
| **B4** | **`build-deploy.ps1` defaults to Debug.** MSVC Debug = no inlining, checked iterators. Publishing Debug timings is indefensible. | `tools/build-deploy.ps1:33-34` |
| **B5** | **Snapshot packets are `new`ed and never `delete`d** in the broadcast path. 60 Hz × N objects of leaked allocations per second. | `DistributedGameServer/DistributedGameServerManager.cpp:245-284` |

### 0.2 Class II — validity defects (the new findings; these are what make experiments *invalid*, not merely unmeasured)

| # | Defect | Consequence for the paper |
|---|---|---|
| **V1** | `PhysicsSystem::IntegrateAccel` (`CSC8503CoreClasses/PhysicsSystem.cpp:535-567`) and `IntegrateVelocity` (`:575-603`) walk `mDynamicObjectList` testing **only** `GetPhysicsObject() != nullptr` (`:537-539`, `:578-580`) — not `HasPhysics()`, not `IsNetworkActive()`. Every server pre-seeds the **entire** world object set (`ServerWorldManager::CreateObjectGrid` builds the same `mCreatedObjectPool` on every server and merely `SetActive(false)`s the out-of-region ones). `mDynamicObjectList` is filled **once**, on the first frame, inside `BroadPhase()`'s `if (mStaticTree.Empty())` branch (`:447`, `:456`). **The smoking gun: the broadphase pair loops *do* skip `!HasPhysics()` (`:461`, `:486`).** So collisions are correctly filtered to owned objects while integration is not. | Per-server physics CPU scales with **total world size**, not with region occupancy — precisely the quantity experiment 1 claims falls as servers are added. **Experiment 1's scaling curve, run today, would be flat by construction, and the flatness would be an artefact of a missing predicate.** Note also that a handed-off object keeps *falling* on the old server: it is deactivated for collision but still integrated under gravity, so its state silently drifts while another server owns it. Most damaging defect in the codebase from a publication standpoint. |
| **V2** | The physics timestep is **not fixed**. `int realHZ` / `float realDT` are **non-const file-scope globals** (`PhysicsSystem.cpp:80-81`) — shared by every `PhysicsSystem` instance in a process — halved on overrun (`:124-128`) and doubled on headroom (`:129-141`) inside `PhysicsSystem::Update` (`:83-142`). The substep loop is `while (mDTOffset > realDT)` with **no iteration cap** (`:93-114`). Linear damping is `1 − 0.4·dt` recomputed **per substep** (`:576`, applied `:588`; angular recomputed at `:599-600`). The member `mGlobalDamping = 0.995f` is dead code. | (a) Under load a server **silently simulates less** rather than taking longer — a flat tick-time curve is again an artefact. (b) Two servers under different load run **different timesteps**, and because damping is a per-substep function of `dt`, their trajectories diverge for reasons unrelated to handoff. **There is no cross-server determinism and therefore no oracle comparison for experiment 4 until this is fixed.** (c) The uncapped substep loop means a stalled server can spiral. |
| **V3** | **Delta snapshots are dead traffic.** `mServerSideLastFullID` lives at `DistributedGameServer/DistributedGameServerManager.h:71` and is assigned in exactly one place — `HandleClientPlayerInputPacket` (`.cpp:182`) — which never fires, because **`GameClient::WriteAndSendClientInputPacket` (`GameClient.cpp:87-91`) has no call sites anywhere in the repo.** Every `DeltaPacket` therefore ships `fullID == 0` forever while the client's `lastFullState.stateID` advances (post-increment at `NetworkObject.cpp:525`, `:541`). After the **first** full snapshot every delta fails `if (p.fullID != lastFullState.stateID) return false;` (`NetworkObject.cpp:440-441`) and is dropped **silently — no counter, no log**. | The client runs on **10 Hz full snapshots only**, while 50 Hz of delta traffic is built, serialised, sent, and thrown away. **Experiment 5 measures nothing real today.** It would report delta bandwidth for packets carrying zero information. It also makes experiment 6's "client-perceived latency" really "age of the last 10 Hz full state". |
| **V4** | **The send rate is 60 Hz, not 20 Hz.** `mTimeToNextPacket += 1.0f / 60.f; //20hz server/client update` (`DistributedGameServerManager.cpp:124`); `mPacketsToSnapshot = 5` gives 1 full : 5 delta ⇒ **10 Hz full + 50 Hz delta**. `docs/NETWORKING.md:86` repeats the wrong figure. | Every bandwidth number in the paper depends on this constant. Correct the code comment and the docs before the methodology section is written from them. |
| **V5** | **`stateHistory` grows unbounded on both ends.** `UpdateMinimumState` (`DistributedGameServerManager.cpp:153-174`) is only reachable from the never-fired input handler, so server-side history is never pruned; the client's `UpdateStateHistory` sits behind the delta guard that always rejects. `docs/NETWORKING.md:88` claims the opposite. | Memory and allocator pressure grow monotonically with run duration, so a long run measures the leak. **A subtlety worth stating precisely:** `GetNetworkState` (`NetworkObject.cpp:620-633`) linear-scans for an exact `stateID`, and because `mServerSideLastFullID` is pinned at 0 the match is at the *front* of the vector — so the scan is O(1) **today**. Fixing V3 without also bounding the history would therefore *introduce* an O(history) scan per object per delta write, converting a memory leak into a CPU leak. **V3 and V5 must be fixed together.** |
| **V6** | **Game servers cross-connect to each other's `DistributedPacketSenderServer`s.** The manager reliably broadcasts the full server roster (`SystemManager::SendStartDataToPhysicsServer`, `SystemManager.cpp:92-105`); each server then loops the roster and opens a `GameClient` to every other server (`DistributedGameServerManager.cpp:418-437`, `ConnectServerToAnotherGameServer` `:482-496`), pumping all N−1 peer clients every frame (`:98-100`). Each therefore receives every other server's **full snapshot stream** and discards it as an unknown object. Handoff packets compound it: `SendFinishTransactionPacket` uses `SendGlobalReliablePacket` (`:466`), a **broadcast to all peers including game clients**, not a point-to-point send. | Total snapshot streams in the system are **N_servers × (N_clients + N_servers − 1)**. O(N²) inbound bandwidth and deserialise cost growing along the exact axis experiment 1 sweeps — indistinguishable in the data from "the system does not scale". |
| **V7** | **No interest management.** The manager reliably broadcasts every server's endpoint to every client (`SystemManager.cpp:86-90`), and each client opens a `GameClient` per server (`DistributedMultiplayerGameScene.cpp:184-213`), receiving the union of all regions. The client then does a **linear `FindNetworkObject` scan per packet** (`DistributedMultiplayerGameScene.cpp:119-126`) — O(objects²) per snapshot. | Client bandwidth and client CPU are functions of total world size, not of what the client can see. A *reported limitation*, and the natural bridge to the plan's "interest management" future work — but it must be **stated**, and client bandwidth must never be presented as if partitioning reduced it. |

### 0.2b Second-order hazards found in the same pass

These do not by themselves invalidate an experiment, but each will corrupt or crash a specific
configuration, and several bite exactly the parameter ranges the plan wants to sweep.

| # | Hazard | Why it matters to the pipeline |
|---|---|---|
| **H1** | **Region borders are computed as `double` in the manager and parsed as `int` on the server.** `GameInstance::CalculateServerBorders` produces floating edges (`DistributedPhysicsServerDto.cpp:89-133`); `DistributedGameServerManager::CreatePhysicsServerBorders` (`:348-388`) `stoi`s them into `PhysicsServerBorderData { int minXVal, maxXVal, minZVal, maxZVal }` (`ServerWorldManager.h:18-24`). | Any world extent not exactly divisible by `numCols`/`numRows` produces **truncated edges ⇒ gaps and overlaps between adjacent regions**. Objects in a gap map to server `-1` and are never handed off; objects in an overlap are claimed by two servers. **Every plan cell must choose `world` extents divisible by `ceil(sqrt(servers))` and `ceil(servers/numCols)`, or the harness must reject the cell.** With `servers ∈ {1,2,3,4,6,8}` this is a real constraint, not a theoretical one. |
| **H2** | **`GetObjectServer` uses fully closed intervals on both axes** (`ServerWorldManager.cpp:284-296`) while `IsObjectInBorder` is half-open on X (`:274-282`). | An object exactly on a shared edge matches two regions; the winner is `std::map` iteration order (lowest ID). Combined with a workload that deliberately parks objects on borders (§D.2), this produces **edge flapping** — repeated handoffs of the same object — which would inflate the handoff-rate axis with an artefact. Detect it: `handoffs.csv` must record `handoff_seq` per object so ping-pong is visible. |
| **H3** | **The broadphase quadtree extent is fixed at 256×256** (`mBroadphaseX/Z = 256`, `PhysicsSystem.h:89-92`; tree built at `PhysicsSystem.cpp:23`, `:42`); `SetNewBroadphaseSize` is never called from the server path. | The default world is ±150 (300 wide) and already exceeds it in one dimension of the tree's half-extent assumption; **any `--world` larger than the default silently falls outside the broadphase**. Plans must either keep world extents within the tree, or the tree must be sized from `--world`. My own example plan below is written accordingly — do not casually enlarge the world to get more objects; enlarge the object count at constant density instead (§D.4). |
| **H4** | **The prediction horizon clamp is dead.** `PhysicsSystem::PredictFuturePositions` (`:213-229`) computes `maxDt = CalculateMaxDt() * 0.90f`, clamps `dt`, and then **passes a hardcoded `0.1f`** to `PredictFutureStateOfObject` (`:226`). `CalculateMaxDt` uses `maxAllowedVelocity = INT_MAX` (`:309-327`), so the clamp is inert anyway. `PredictFutureStateOfObject` also adds gravity **regardless of `mApplyGravity`** (`:191-193`). | The `--predict-horizon` flag experiment 4 needs replaces `:226`, not the clamp. The gravity inconsistency means prediction and integration disagree whenever gravity is off — a silent systematic bias in the handoff-error metric. Fix before measuring. |
| **H5** | **Handoff is send-and-release with a one-tick delay.** The flag is set in `ServerWorldManager::Update` (`:157`) but the packet is only built and sent in `DistributedGameServerManager::UpdateGameServerManager` (`:103`) on the *next* pass, and the sender deactivates the object immediately (`:449-453`) without waiting for an ack. The ack path is inert: `ServerWorldManager::HandleTransitionHandshakeReceived` has an **empty body** (`:207-210`), `DistributedGameServerManager::HandleTransitionHandshakePacketReceived`'s only action is commented out (`:316`), `NetworkObject::OnTransitionHandshakeReceived` is never called, and `mIsWaitingHandshake` is set but never read. | "Handoff latency" must be defined against something real. Today the only measurable interval is *send → resume*; there is no completion event. The built-in one-tick send delay is a **fixed additive bias** in every handoff measurement and must be reported (or removed by sending in the same pass). |
| **H6** | **Lifetime bugs that will crash unattended runs.** `SystemManager::GetPhysicsServerDataList` returns a reference to a **function-local** vector (`SystemManager.cpp:235-247`) and `CheckIsGameStartable` reads it. `GameInstance::CalculateServerBorders` `new`s a `GameBorder` per call and returns it **by reference** (`:89-133`) — leak. `ServerWorldManager::CalculateIncomingObjectOffsetPosition` returns a reference to a stack local (`:298-316`) and is dead code. `PhysicsSystem::ClearForces` (`:610-616`) dereferences `GetPhysicsObject()` with **no null check**. | An unattended matrix runs these paths thousands of times. Random crashes will be attributed to load rather than to undefined behaviour, and the harness will correctly but uselessly mark cells `failed`. Fix before the first overnight run. |
| **H7** | **`GameInstance::CalculatePhysicsServerBorders` loops `for (i = serverIDBuffer; i < mServerCount; i++)`** (`DistributedPhysicsServerDto.cpp:35-42`) where the bound should be `serverIDBuffer + mServerCount`; and `DistributedPhysicsServerData::SetIsAllClientsConnectedToServer` has an **empty body** (`:157-159`). | A second game instance in the same manager process gets too few borders (or none). **The harness must therefore create exactly one instance per manager process and tear the manager down between cells** — which the design already does, but for a reason worth recording. |
| **H8** | **`BasicNetworkMessages` is an unnumbered enum** (`NetworkBase.h:8-52`, distributed tail at `:38-51`). | Any new message type (`SnapshotAckPacket` for experiments 5/6) **must be appended after `AddTestObjectsToTheWorld` (`:51`)**, never inserted, or every wire value renumbers and old captures/datasets become unreadable. Write this into the dataset documentation. |

### 0.3 Experiment validity verdict

| Exp | Plan | Status today | Blocking fixes (all C++) |
|---|---|---|---|
| **2** Single-server baseline | tick-time reference line for all figures | **RUNNABLE SOONEST.** V1/V6/V7 are inert at N=1 (one server owns everything, no peers). Blocked only by B1–B4 and V2/V5. | B1 (workload), B2 (object cap), B3 (duration/exit/flush), B4 (Release), V2 (`--fixed-hz`), V5 (bound `stateHistory`) |
| **1** Scaling 1→8+ | servers vs tick time / throughput | **INVALID.** V1 makes per-server cost independent of partitioning; V6 makes inter-server bandwidth grow as N²; V2 hides load as reduced fidelity; V5 adds a duration-dependent trend. | V1, V6, V2, V5 + all of exp 2's |
| **3** Object sweep 100→1000+ /server | objects vs tick time, bandwidth | **BLOCKED** (cap, B2) and, at N>1, **INVALID** for the same reason as exp 1 (V1 — "per server" is meaningless when every server simulates everything). Bandwidth axis has no counters at all. V7 confounds the client-side bandwidth reading. | B2, V1, byte counters, V5; disclose V7 |
| **4** Handoff accuracy, many crossings | position-error CDF/boxplot | **NOT MEASURABLE.** No workload ⇒ zero crossings (B1). Zero handoff instrumentation. The ack half of the handshake is inert (`ServerWorldManager::HandleTransitionHandshakeReceived` has an empty body at `:207-210`; the receiver's only action in `DistributedGameServerManager::HandleTransitionHandshakePacketReceived` is commented out at `:316`; a `//TODO: send handshake packet` sits at `ServerWorldManager.cpp:194`), so handoff is send-and-release and there is no completion event to time against. The oracle/ground-truth definition additionally requires cross-run determinism, which V2 forbids. | B1, handoff event hooks, V2, collision isolation |
| **5** Snapshot full vs delta | bytes/bandwidth comparison | **INVALID — measures dead traffic.** V3 means 100 % of deltas after the first full are discarded by the client. V4 means the documented rate is wrong. V6 pollutes any total-bytes accounting with inter-server noise. No byte counters exist. | V3 (repair the full-ID/ack path), V4 (doc + constant), V6, byte counters, `--snapshot-mode` |
| **6** Client latency CDF (optional) | client-perceived latency CDF | **NOT MEASURABLE.** No timestamp in any packet, no ack path, no client-side clock, and `fps`/`net` in `@@STAT` are hardwired 0.00. | Echo/ack scheme (§B.4), V3 (otherwise you measure 10 Hz fulls), disclose V7 |

**Read that table as the paper's risk register.** Four of six experiments would, if run today, produce
plausible-looking numbers that are wrong. The pipeline's first job is not automation — it is to make
the measurements *true*.

---

## A. Metric gap analysis

### A.1 What `@@STAT` actually emits

One emitter: `TelemetryReporter::MaybeEmit`
(`CSC8503CoreClasses/DistributedSystemCommonFiles/TelemetryReporter.cpp:25-67`), rate-limited to one
line per **500 ms** (`kInterval`), all floats formatted `std::fixed << setprecision(2)`. Called from
four sites — `DistributedPhysicsManager/ProgramStart.cpp`, `PhysicsServerMidware/ProgramStart.cpp`,
`DistributedGameServer/ServerStarter.cpp`, `CSC8503/DistributedClientStart.cpp`.

| Role | Keys | Reality |
|---|---|---|
| manager | `midwares clients instances` | Live, but **counters are never decremented on disconnect** — they are high-water marks, not occupancy. A midware that dies mid-run leaves the count inflated, so the harness cannot use them as a liveness gate without also tracking role process state. |
| midware | `id manager` | `id` always `0` (constructed with the default). `manager` **always `0`** — `Profiler::SetIsConnectedToGameManager` is never called anywhere. |
| server | `id objs total phys world predict full delta game` | `objs` is an **instantaneous per-tick sample** happening to be read at the 500 ms boundary — not a mean. `total` is `mNetworkIdBuffer - 10`, a **monotonic ID counter**, not a live object count. `world` times `GameWorld::UpdateWorld`, which does nothing but optionally shuffle two vectors (both flags default false) — **it times an empty function**. `phys`/`predict`/`full`/`delta` are **last-sample-wins** milliseconds. |
| client | `id fps net game` | `id` always `0`. `fps` **always 0.00** (`Profiler::SetFramesPerSecond` is only called from the unused imgui path). `net` **always 0.00** (`SetNetworkTime` called nowhere). |

Additional format-layer damage: `Profiler` stores times as `float` **milliseconds** and `@@STAT`
prints `%.2f`. A 40 µs physics step prints `0.04`; anything under 5 µs prints `0.00`. **Sub-10 µs
resolution is destroyed before the number leaves the process.** Nothing emits a wall-clock timestamp
— the launcher timestamps on receipt, adding pipe + WPF-dispatcher latency of unknown magnitude.

`Profiler` itself is a bag of static globals with plain setters: no histograms, no percentiles, no
ring buffer, **no thread safety**. Memory *is* computed in `Profiler::Update`
(`GlobalMemoryStatusEx` + `GetProcessMemoryInfo`, `Profiler.cpp:220-238`) but is only exposed as a
`std::string` in MB and is never emitted.

**Net: 13 keys, 3 hardwired zero, 2 hardwired constant, 1 timing an empty function, 2 semantically
mislabelled, all scalars, none aggregated, at 2 Hz, quantised to 10 µs.** Nothing in that sentence
supports a percentile.

### A.2 Experiment → metric gap table

Columns: what the plan needs → what exists → what is missing → **exact emit site for the new metric**.
New C++ files live in `CSC8503CoreClasses/DistributedSystemCommonFiles/`.

| Exp | Required metric | Exists today | Missing | Emit site (file :: class/function) |
|---|---|---|---|---|
| **1, 2, 3** | Per-tick wall time distribution (p50/p95/p99) | `phys` — last sample, 2 Hz, `%.2f` ms | Per-tick record at integer µs | `DistributedGameServer/ServerWorldManager.cpp::ServerWorldManager::Update` (the three `Profiler::Set*Time` call sites already bracket the work — add a sink write); outer tick wall time in `DistributedGameServer/ServerStarter.cpp` tick lambda |
| **1, 2, 3** | **Actual substep count and `realHZ` per tick** | nothing | `iteratorCount` is computed and discarded | `CSC8503CoreClasses/PhysicsSystem.cpp::PhysicsSystem::Update` — **and `realHZ`/`realDT` must first become members, not file-scope globals** (V2) |
| **1, 3** | Objects **actually integrated** vs objects **owned** | `objs` (instantaneous), `total` (ID counter) | Both, per tick, correctly defined; the gap between them *is* the V1 defect and must be visible in the data | `ServerWorldManager::Update`; the integrated count from `PhysicsSystem::IntegrateAccel` |
| **1** | Aggregate throughput (object-substeps/s) | nothing | derived = `owned × substeps / tick_s`, summed across servers | derived in analysis from the tick stream |
| **1, 6** | **Inter-server ingress** (the V6 O(N²) traffic) | nothing | bytes and packets received from *peer servers*, split from client traffic | `CSC8503CoreClasses/GameServer.cpp` receive path + `DistributedPacketSenderServer` — count `Full_State`/`Delta_State` arriving at a *server* peer. This must be reported as **zero** after the V6 fix; before the fix it is the headline confound. |
| **3, 5** | Bytes/s and packets/s out, **split full vs delta** | nothing | application bytes and wire bytes | **`CSC8503CoreClasses/GameServer.cpp::SendGlobalPacket` / `SendGlobalReliablePacket`** — the single choke point where `packet.GetTotalSize()` is already computed. Key by `packet.type`. The unused `mOutgoingDataRate` / `mIncomingDataRate` members in `GameServer.h` were clearly meant for exactly this. Cross-check against ENet's own `host->totalSentData` for wire-level bytes including framing. |
| **5** | **Delta acceptance rate** (the V3 detector) | nothing | count of deltas written vs deltas *applied* | server: `NetworkObject::WriteDeltaPacket`; client: `NetworkObject::ReadDeltaPacket` at the `p.fullID != lastFullState.stateID` guard (`NetworkObject.cpp:440`) — increment `accepted` / `rejected`. **This counter is the acceptance test for the V3 fix and must appear in every run's manifest.** |
| **5** | Snapshot build cost, per mode | `full`/`delta` last-sample ms | per-broadcast record | `DistributedGameServer/DistributedGameServerManager.cpp::UpdateGameServerManager` / `BroadcastSnapshot` (timing exists; only the sink is missing) |
| **4** | Per-handoff record: object, src/dst, sim tick, authoritative pos, predicted pos, velocity, resume pos, latency | **nothing** | everything | **sender:** `DistributedGameServerManager::HandleObjectTransitions` + `SendFinishTransactionPacket` (where `StartSimulatingObjectPacket` is built), and `NetworkObject::FinishTransitionToNewServer`. **receiver:** `ServerWorldManager::StartHandlingObject` (`:165-205`) — both the predicted and authoritative positions are simultaneously in scope at `:182-186`, which makes it the natural instrumentation point. |
| **4** | Handoff **completion** latency | nothing | the ack half of the handshake is stubbed | `SendTransactionHandshakePacket` / `HandleTransitionHandshakePacketReceived` — must be *implemented* (or the metric redefined as one-way; see §A.4) |
| **4** | Handoff **rate** (the independent variable) | nothing | count/s per server, in-flight, failed/duplicate | `DistributedGameServerManager::HandleObjectTransitions` |
| **4 (E2)** | Per-object position trace keyed by `sim_tick` | nothing | opt-in trace stream | `ServerWorldManager::Update`, behind `--trace-objects` / `--trace-stride` |
| **6** | Client-perceived latency samples | `net` = 0.00, always | timestamped snapshot + echo | server stamp in `NetworkObject::WriteFullPacket`; client echo in `CSC8503/DistributedMultiplayerGameScene.cpp`; server closes the loop in `DistributedPacketSenderServer`. New `BasicNetworkMessages` enum entry in `CSC8503CoreClasses/NetworkBase.h`. |
| **6** | Snapshot **age in server ticks** (clock-free) | nothing | `server_tick_now − server_tick_of_applied_state` | client: `DistributedMultiplayerGameScene` on apply |
| **all** | CPU %, RSS, private bytes, per role | computed, never emitted, string-only | numeric getters + `GetProcessTimes` | `CSC8503CoreClasses/Profiler.{h,cpp}` — add numeric accessors alongside the existing `std::string` MB ones; sample at 2 Hz off the tick path |
| **all** | Run identity on every record | nothing | `run_id`, `role`, `role_id`, `sim_tick`, `t_us`, `phase` | new `RunClock` + `ExperimentConfig` in `DistributedSystemCommonFiles/` |
| **all** | **Wall-clock timestamp at emission** | nothing (launcher timestamps on receipt) | monotonic µs since role start, plus one QPC↔UTC anchor pair per run | `RunClock` |

### A.2b Exact hook sites, confirmed by inspection

The gap table above names classes; these are the lines. Every one of these was verified, and each is a
one-or-two-line insertion.

**Per-tick timing — the tick order is already instrumented, only the sink is missing.**
`ServerWorldManager::Update(float dt)` (`DistributedGameServer/ServerWorldManager.cpp:75-114`) runs:
object update loop `:83-92` (`Profiler::SetObjectsInBorders(activeObjCount)` `:93`) → `UpdateWorld`
`:96` (`SetWorldTime` `:99`) → `PredictFuturePositions` `:102` (`SetPhysicsPredictionTime` `:105`) →
`mPhysics->Update` `:108` (`SetPhysicsTime` `:111`) → `CheckPositionOutOfServerBoundaries` `:113`
(**untimed — add a timer**). The outer tick lambda is `ServerStarter.cpp:106-113`, shared by the
headless and windowed paths, and calls `reporter.MaybeEmit` at `:112`. Note the windowed path skips
frames with `dt > 0.1f` (`:132-135`) while the headless path does not — **another reason all
measurement runs must be headless**, and a reason to record `dt` per tick.

**Outbound bytes — one choke point covers every role.** `GameServer::SendGlobalPacket`
(`CSC8503CoreClasses/GameServer.cpp:66-71`; `enet_packet_create(&packet, packet.GetTotalSize(), 0)` at
`:68`) and `SendGlobalReliablePacket` (`:60-64`, `:61`). Client side: `GameClient::SendPacket`
(`GameClient.cpp:93-97`). Per-snapshot split by mode is better taken at
`DistributedGameServerManager::BroadcastSnapshot` (`:245-270`), specifically the
`SendGlobalPacket(*newPacket)` at `:266` — which is also where the **missing `delete newPacket`**
belongs (B5). `enet_host_broadcast` fans out to N peers, so wire bytes = payload × connected peers.

**Inbound bytes — also one choke point.** `NetworkBase::ProcessPacket`
(`CSC8503CoreClasses/NetworkBase.cpp:27-44`) is the single funnel every inbound packet passes through
on **every** role; `packet->type` and `GetTotalSize()` are both in scope. For *true wire* bytes use
`event.packet->dataLength`, in scope at `GameServer.cpp:118`, `GameClient.cpp:73`, and
`DistributedPacketSenderServer.cpp:36`. **Zero-code aggregate cross-check:** `netHandle` is
`protected` in `NetworkBase.h:147`, so any subclass can read ENet's own
`totalSentData / totalSentPackets / totalReceivedData / totalReceivedPackets` (`enet/enet.h:386-389`)
— UDP-level bytes including ENet framing and retransmits. **Report both application and wire bytes.**

The placeholders `int mIncomingDataRate; int mOutgoingDataRate;` at `GameServer.h:37-38` are declared
and never touched anywhere in the repo. They are exactly this metric, half-built.

**Packet sizes (x64 MSVC) — the analytical model the measurements must agree with.**
`GamePacket` = 4 B (`short size; short type;`, `NetworkBase.h:63-79`). `NetworkState`
(`NetworkState.h:8-33`) has a **`virtual ~NetworkState()`**, hence an 8-byte vptr, hence
`sizeof = 72`. Therefore `sizeof(FullPacket) = 88` (declared `size` = 84) and
`sizeof(DeltaPacket) = 24` (declared `size` = 20). Steady state per object per client at 60 Hz:
`10×88 + 50×24 = 2080 B/s`. **If `WriteDeltaPacket` fails, `WritePacket` silently falls back to a
full packet** (`NetworkObject.cpp:427-435`, fallback at `:429-431`), giving `60×88 = 5280 B/s` — a
2.5× bandwidth swing with no error surfaced. Experiment 5 must count fallbacks as a first-class
metric, not just full/delta. **Verify both `sizeof` values at runtime with a `static_assert` before
publishing.** Note also that ENet coalesces commands into datagrams up to the MTU, so *packets ≠
datagrams* — the wire-byte figure must come from ENet's counters, not from multiplying.

**Handoff.** Sender flag: `NetworkObject::FinishTransitionToNewServer` (`NetworkObject.cpp:592-596`),
set from `ServerWorldManager::CheckPositionOutOfServerBoundaries:157`. Sender send + release:
`DistributedGameServerManager::HandleObjectTransitions` (`:446-455`) and
`SendFinishTransactionPacket` (`:457-467`), which overwrites `lastFullState.position` with the current
transform then packs `predictedPosition` from `GetPredictedPosition()`
(`NetworkObject.cpp:333-357`, `:343`) plus velocity/force/torque/inertia. Receiver:
`ServerWorldManager::StartHandlingObject` (`:165-205`) — pool lookup `mCreatedObjectPool.at(objectID)`
at `:166` (**`.at()` throws on an unknown ID — a crash path the harness will see as a `failed` cell**),
`SetPosition(lastNetworkState.predictedPosition)` at `:182`, the name-swapped
`SetPredictedPosition(lastNetworkState.position)` at `:185`, velocity/force restore `:189-191`
(torque commented out at `:192`), `SetActive(true)` `:197`. **Both the authoritative and the predicted
position are in scope at `:182-186` — that is the instrumentation point, and no new plumbing is
needed to obtain the values.**

**Client.** Receive loop `DistributedMultiplayerGameScene::UpdatePhysicsClients` (`.cpp:176-182`);
snapshots become visible state at `HandleFullPacket` `:160` and `HandleDeltaPacket` `:168`, which is
where a client-side timestamp must be taken. **The client has no clock at all** — no `std::chrono`, no
accumulated time; `GameClient::mTimerSinceLastPacket` is a frame counter, not a clock. A `RunClock`
must be introduced on the client before any latency number exists.

**Per-object identity across servers** is `NetworkObject::networkID`, allocated sequentially from 10
(`ServerWorldManager.cpp:17`, `:117-119`) and identical on every server *only because creation order
is identical*. That is the join key for the oracle comparison, and it is another reason workload
determinism (§D.1) must not depend on iteration order.

**Free lunch, worth noting:** `FullPacket` already carries `serverID` (read client-side at
`NetworkObject.cpp:477`) and `fullState.predictedPosition`. A client therefore already observes
`(objectID, owning server, actual position, predicted position)` at 10 Hz **without any server
change** — useful for a quick sanity check of ownership churn before the C++ work lands, though not
sufficient for any published figure.

### A.3 Experiment 4 — what "handoff position error" can defensibly mean

The obvious definition ("distance from where the object should have been") is not directly
measurable, because nothing in the system knows where the object should have been. Two operational
definitions; implement both, in this order.

**What is available at the handoff point.** The sender builds `StartSimulatingObjectPacket` carrying
the latest `NetworkState` (authoritative position/orientation), a `predictedPosition` from
`Transform::GetPredictedPosition()`, and the full physics state (linear/angular velocity, force).
The receiver, in `ServerWorldManager::StartHandlingObject`, resumes the object **at the prediction**
and stores the authoritative position as the predicted one (the two are swapped relative to the
naming). So at the receiver, `p_auth`, `p_pred`, `v`, and `p_resume` are all in scope on the same
line — no new plumbing is needed to *obtain* them, only to *record* them.

The prediction horizon is a hardcoded `0.1f` passed at the call site
`PhysicsSystem::PredictFuturePositions:226` (the surrounding `maxDt` clamp is computed and then
ignored, and is inert anyway — H4). Making it `--predict-horizon` costs nothing and converts
experiment 4 from a single number into an **error-vs-horizon curve** — a materially stronger result,
and one the thesis did not have.

- **E1 — continuity error.** Ships first; cheap.
  `err_e1 = ‖ p_resume − (p_send + v_send · Δt_handoff) ‖`
  All three terms exist at `StartHandlingObject`. `Δt_handoff` is a one-way interval between two
  processes; §B.4 establishes that on a **single machine** this is legitimate to sub-microsecond
  precision. On multi-machine runs record it as `NaN` and fall back to E2. E1 answers *"did the
  prediction land where the object was actually going"* — the qualitative claim of the thesis'
  single-object figure, now as a distribution over thousands of crossings.

- **E2 — oracle divergence.** The publication-grade metric.
  Run the identical seeded workload twice: once at **N=1** (experiment 2 — no handoffs at all, hence
  ground truth) and once at N servers. Both write per-object traces keyed by **`sim_tick`, never wall
  clock**. Then `err_e2(obj, tick) = ‖ p_dist(obj, tick) − p_base(obj, tick) ‖`, and the handoff-error
  CDF is `err_e2` sampled at the first tick after each handoff. This also yields a second figure the
  plan does not have — **divergence growth vs. time since handoff** — which is the strongest available
  answer to "does predictive handoff preserve continuity".

  **E2 has two hard prerequisites, and V2 blocks one of them outright:**
  1. **Fixed timestep (V2).** With `realHZ`/`realDT` as mutable file-scope globals that differ per
     server under differing load, and damping applied as `1 − 0.4·dt` per substep, two runs — indeed
     two *servers within one run* — integrate different equations. `--fixed-hz` pinning `realDT`, plus
     a fixed-rate outer loop, is **mandatory**, and the harness must gate on it (§C.4).
  2. **Collision isolation.** Objects are partitioned differently across servers, so narrow-phase pair
     ordering differs and colliding trajectories diverge chaotically regardless of handoff quality —
     the metric would measure chaos. **The experiment-4 workload must therefore be collision-free
     between test objects** (distinct Y bands or a non-colliding layer). State this in the paper's
     methodology as the standard isolation it is; do not bury it.

  **Determinism must be *validated*, not assumed.** Before investing in E2: run the same seed and
  server count twice and require bit-identical traces. If that gate fails, E2 is not defensible and
  experiment 4 ships as E1 only. Put this check in the harness (§C.4), not in a human's memory.

**One more item to resolve before the artefact is published:**
`CalculateIncomingObjectOffsetPosition` (`ServerWorldManager.cpp:298-316`) returns a reference to a
function-local `Vector3` (dangling — undefined behaviour), has both X branches commented out, has two
Z branches that do the same thing, and **has no call sites**. Implement it or delete it. A reviewer
reading the artefact will find it, and `docs/SPATIAL-PARTITIONING.md:52-54` currently describes it as
if it were live.

### A.4 The explicit "impossible without changing C++" list

No harness, PowerShell, or pandas work substitutes for any row below.

| Required change | File / symbol | Unblocks |
|---|---|---|
| `--duration N --warmup N`, clean `exit(0)`, sink flush on shutdown, graceful stop predicate | `DistributedSystemCommonFiles/HeadlessRunner.{h,cpp}` + all four role tick lambdas | **everything** (B3) |
| Fixed-rate sim loop replacing the `sleep_for(1 ms)` spin | `HeadlessRunner.cpp` | headless timings currently do not correspond to windowed ones; CPU% is meaningless |
| `realHZ`/`realDT` become **members** of `PhysicsSystem`; add `GetRealHZ()`, `GetLastSubstepCount()`; add `--fixed-hz` | `CSC8503CoreClasses/PhysicsSystem.{h,cpp}:124-141` | V2 → exps 1, 2, 3, 4(E2) |
| **`HasPhysics()` / `IsNetworkActive()` predicate in `IntegrateAccel` / `IntegrateVelocity`** (or exclude inactive objects from the integration list) | `CSC8503CoreClasses/PhysicsSystem.cpp` | **V1 → experiment 1 is meaningless without this** |
| Stop game servers subscribing to peer servers' snapshot streams (or filter at ingress) | server-to-server client mesh setup in `DistributedGameServerManager` | V6 → exps 1, 3, 5 |
| Repair the delta baseline: write `mServerSideLastFullID` on a real ack, not in `HandleClientPlayerInputPacket`; make `UpdateMinimumState` reachable | `CSC8503CoreClasses/NetworkObject.cpp` (guard at `:440`), `DistributedGameServerManager` | **V3 → experiment 5**; also V5 |
| Bound `stateHistory` (ring or prune-on-ack) and replace the linear scan in `GetNetworkState` | `NetworkObject.{h,cpp}` | V5 → all long runs |
| `--snapshot-mode full\|delta\|adaptive` replacing the unconditional `mPacketsToSnapshot = 5` | `DistributedGameServerManager::UpdateGameServerManager` | experiment 5 needs a full-only arm |
| Byte/packet counters keyed by packet type | `GameServer::SendGlobalPacket` (both overloads), `GameServer.h` | exps 3, 5 |
| `WorkloadDriver` — autonomous, seeded object motion | new `DistributedSystemCommonFiles/WorkloadDriver.{h,cpp}`, hooked in `ServerWorldManager::Update` | B1 → exps 1, 3, 4 |
| Object placement decoupled from the 10×10 grid; `--objects` honoured to 1000+ | `ServerWorldManager::CreatePlayerObjects` / `CreateObjectGrid` | B2 → experiment 3 |
| Explicit `std::mt19937` seeding; remove bare `rand()` | `ServerWorldManager.cpp:237` | reproducibility |
| Handoff event hooks at both ends; implement or remove the ack | `HandleObjectTransitions`, `StartHandlingObject`, `HandleTransitionHandshakePacketReceived` | experiment 4 |
| `--predict-horizon` replacing the hardcoded `0.1f` argument | `PhysicsSystem::PredictFuturePositions:226` | experiment 4 becomes a curve |
| `senderTicks` in `FullPacket`; `SnapshotAckPacket`; new enum entry; client echo | `NetworkBase.h`, `NetworkObject.{h,cpp}`, `DistributedMultiplayerGameScene.cpp`, `DistributedPacketSenderServer.cpp` | experiment 6 (and it finally gives the delta path a real baseline — same fix as V3) |
| Numeric memory/CPU getters | `Profiler.{h,cpp}` | resource stream |
| `delete newPacket` after `SendGlobalPacket` | `DistributedGameServerManager::BroadcastSnapshot:266` | B5 |
| `-Config Release` default | `tools/build-deploy.ps1:33-34` | B4 |
| `MetricSink`, `RunClock`, `ExperimentConfig`, `Histogram` | new, `DistributedSystemCommonFiles/` | §B |
| **A clock on the client** (it has none — no `std::chrono`, no accumulated time) | `CSC8503/DistributedMultiplayerGameScene`, `DistributedClientStart.cpp` | experiment 6 |
| Integer-safe border computation, or harness-side rejection of indivisible world extents | `DistributedPhysicsServerDto.cpp:89-133` ↔ `DistributedGameServerManager::CreatePhysicsServerBorders:348-388` | H1 — gaps/overlaps between regions |
| Consistent interval convention between `IsObjectInBorder` and `GetObjectServer` | `ServerWorldManager.cpp:274-296` | H2 — edge flapping |
| Broadphase extent derived from `--world` (or plans constrained to the fixed 256) | `PhysicsSystem.h:89-92`, `PhysicsSystem.cpp:23,42` | H3 — larger worlds fall outside the quadtree |
| Gravity applied consistently in prediction and integration | `PhysicsSystem::PredictFutureStateOfObject:191-193` | H4 — systematic bias in handoff error |
| Fix the four dangling-reference / missing-null-check sites | `SystemManager.cpp:235-247`; `DistributedPhysicsServerDto.cpp:89-133`; `ServerWorldManager.cpp:298-316`; `PhysicsSystem::ClearForces:610-616` | H6 — random crashes in an unattended matrix |
| `mCreatedObjectPool.at()` → checked lookup on the handoff receive path | `ServerWorldManager::StartHandlingObject:166` | H6 — an unknown object ID throws |
| Append (never insert) the new message type | `NetworkBase.h:51` | H8 — wire-value stability of the published dataset |

**Two correctness notes that change the *numbers*, not just the plumbing** — both must appear in the
paper or a reviewer will find them:

- Delta packets encode position deltas as `char pos[3]` — a **truncating cast to one signed byte per
  axis**. Delta replication is lossy at 1 world-unit granularity and undefined for |Δ| ≥ 128. Any
  "delta is cheaper" result must be reported **alongside its fidelity cost**. Consider adding a
  16-bit fixed-point variant and reporting both.
- Several control packets set `size = sizeof(ThePacket)` instead of
  `sizeof(ThePacket) − sizeof(GamePacket)`, and `GetTotalSize()` adds `sizeof(GamePacket)` on top —
  so those packets are sent with an inflated declared length. It does not affect the snapshot path
  experiment 5 measures, but it will show up in total-bytes accounting and must be fixed or excluded.
- `docs/NETWORKING.md:86, 88` currently state the send rate is 20 Hz (V4) and that `UpdateMinimumState`
  prunes acknowledged history (V5). **Both statements are false and must be corrected before the
  paper's system-design section is written from them.**

---

## B. High-rate measurement

### B.1 Why `@@STAT` cannot be the data path

2 Hz over a stdout pipe cannot produce a p99, and the transport is actively hostile: a headless
midware forwards **every** game-server line, so 8 servers emitting per-tick records would funnel
through one pipe, one `Process.OutputDataReceived` callback, and one WPF dispatcher
(`RoleProcess.cs:60`, `MainWindow.xaml.cs:133`). The instrument would perturb the measurement by
orders of magnitude more than the effect being measured.

**Rule: per-tick data never touches stdout.** `@@STAT` is retained *only* as a liveness/progress
heartbeat for the harness — "the run is healthy, the game has started, warm-up is over". It is not a
data source for any figure.

While it is retained, three cheap repairs make it usable as a gate: call
`Profiler::SetIsConnectedToGameManager` so the midware's `manager` key is real; pass the real id to
the midware/client `TelemetryReporter` constructors; and add `run_id` + a monotonic sequence number so
the harness can detect gaps.

### B.2 `MetricSink` — pre-allocated, POD, flushed once

```
CSC8503CoreClasses/DistributedSystemCommonFiles/MetricSink.{h,cpp}
```

- **One sink per stream:** `ticks`, `snapshots`, `handoffs`, `latency`, `resource`, `trace`.
- **Records are fixed-size PODs.** No strings, no `std::string`, no allocation on the hot path.
  `TickRecord { u64 sim_tick; u64 t_us; u32 tick_us, phys_us, world_us, predict_us, border_us;
  u16 substeps, real_hz; u32 owned, integrated, border_objs; u8 phase; }` ≈ 56 B.
  Recording a tick is **one struct store into a pre-sized buffer**.
- **Backing store is a `std::vector<T>` `reserve()`d at construction** to
  `ceil(duration_s × expected_rate × 1.2)`. 1 kHz × 300 s × 56 B ≈ **17 MB**. Trivial. Zero
  allocation, zero I/O, zero locking inside the measured window.
- **Overflow policy: drop and count.** If full, bump `dropped` and return. **Never reallocate
  mid-run** — a 17 MB `realloc` inside a physics tick would manifest as a p99.9 spike *caused by the
  instrument*. `dropped` is written to `run.json` and **any non-zero value fails the run** (§C.4).
- **Flush once, in the shutdown path** (which B3 must create): write the whole buffer to
  `<metrics-dir>/<role><id>/<stream>.csv`. CSV at these volumes is fine, is self-describing, and is
  what the OSS & Datasets track wants to receive.
- **Thread safety by construction, not by locking.** Each sink is owned by exactly one thread. The
  snapshot path is currently inline on the main loop (the dedicated sender thread is commented out);
  if it is ever re-enabled, give that thread its own sink instance rather than adding a mutex — a
  mutex on the hot path reintroduces exactly the perturbation this design avoids. Note `Profiler`'s
  static globals are **not** thread-safe today; `MetricSink` must not inherit that pattern.

**Sampling rates:**

| Stream | Rate | Rationale |
|---|---|---|
| `ticks` | **every tick** | this is the distribution the paper needs; it costs one 56-byte store |
| `snapshots` | every broadcast (60 Hz — V4) | bytes are already computed at the send site |
| `handoffs` | every event | rare by construction; both ends write |
| `latency` | every echoed snapshot, capped ~200 Hz | |
| `resource` | 2 Hz | `GetProcessMemoryInfo`/`GetProcessTimes` are syscalls — keep them off the tick path |
| `trace` | every K-th tick (`--trace-stride`, default 6 ⇒ 20 Hz at 120 Hz sim) | volume control; 1000 objects × 20 Hz × 300 s × 16 B ≈ 96 MB — acceptable, and it *is* the dataset for the OSS track |

**Alternative considered and rejected:** a separate telemetry socket/named pipe per role. It moves
the cost off stdout but keeps a syscall and a serialisation on the hot path, adds a second failure
mode (a stalled reader back-pressuring the simulation), and buys nothing the flush-at-end path does
not already give for bounded runs. Reserve it for nothing.

**One escape hatch, for long-soak runs only:** the same class supports a ring buffer plus a detached
flusher thread that memcpys completed 64 KB chunks to disk; the writer only bumps a head index. Use
it **only** where the pre-allocation would not fit, because flush-at-end is the variant that is
*provably* non-perturbing and that is the property worth defending in a methodology section.

### B.3 Histograms

Add `Histogram.h` — fixed log-spaced buckets (≈1 µs to 1 s, 3 significant digits; an HDR-histogram
lite). Two uses: (i) `@@STAT` can be upgraded to carry live p50/p95/p99 for the dashboard without
sending per-tick lines; (ii) histograms are **mergeable across repetitions**, which is exactly what is
needed when pooling N reps into one CDF, and they give the long-soak mode a bounded-memory fallback.
Histograms complement, never replace, the raw stream — a reviewer may want the raw samples, and the
dataset track certainly does.

### B.4 Clock synchronisation and what precision is defensible

**What exists.** `GameTimer` and the inline timing in `ServerWorldManager::Update` use
`std::chrono::high_resolution_clock`, which on MSVC is `steady_clock`, QPC-backed, monotonic, ~100 ns
resolution. That is adequate. Two things destroy it downstream and must be fixed:

1. `GameTimer::timeDelta` is a **`float`** in seconds. Fine for a per-frame delta; **never accumulate
   run time in it.** `RunClock` keeps `uint64` microseconds.
2. `Profiler` stores `float` milliseconds and `@@STAT` prints `%.2f` (§A.1). Record integer
   microseconds in `MetricSink` and leave `Profiler` alone for the on-screen profiler.

**Same machine — one-way latency IS defensible, and this matters.** On Windows, `steady_clock` is
QPC-derived, and QPC is documented as system-wide consistent across processes on a given machine:
same source, same origin. So a timestamp taken in a sending process and one taken in a receiving
process on the same box are directly comparable to well under a microsecond. Record
`QueryPerformanceFrequency` and one QPC↔UTC anchor pair per role in `run.json` so the claim is
auditable. **This is what legitimises E1's `Δt_handoff` (§A.3) and is one of several reasons
experiments 1–5 should be run single-machine.**

**Cross-machine — do not claim one-way latency.** Windows' default w32tm offsets are tens of
milliseconds, the same order as the quantity being measured. Three schemes, in preference order:

1. **Server-side echo (recommended; clock-free).** Add `uint64 senderTicks` to `FullPacket`, stamped
   in `NetworkObject::WriteFullPacket` (`:533-547`). The client echoes the highest `senderTicks` it
   has applied in a small `SnapshotAckPacket` (**appended** to `BasicNetworkMessages` after
   `NetworkBase.h:51` — see H8). The **server** computes `rtt = now − echoed_senderTicks` entirely in
   its own clock. **No cross-clock comparison ever occurs**, so the number is exact regardless of
   topology. Report the **RTT CDF** as the headline for experiment 6. This is the same ack the delta
   path needs for V3 and V5 — one mechanism, three fixes, which is why it should be built once, in
   Increment 4, rather than twice.

   *Rejected alternative — ENet's own RTT.* `ENetPeer::roundTripTime` exists (`enet/enet.h:302`) and
   the client's peer is reachable with one added getter (`GameClient.h:87` holds the only `ENetPeer*`
   in the codebase; the server retains none — `GameServer::mPeers` is an `int` array, so the server
   would have to walk `netHandle->peers[i]`). **But ENet only updates `roundTripTime` from *reliable*
   packet acknowledgements, and the snapshot stream is sent unreliably** (`GameServer.cpp:68`, flags
   `0`). Without adding a periodic reliable ping the value would reflect only sparse control traffic,
   not the snapshot path being measured. Use it as a **cross-check** on the echo numbers, not as the
   measurement. `packetsSent` / `packetsLost` / `packetLoss` (`enet.h:283-285`) are free and worth
   recording regardless — loss is otherwise invisible on an unreliable stream.
2. **RTT/2 as a one-way estimate**, reported as such: *"one-way latency is estimated as RTT/2, which
   assumes path symmetry; on a switched LAN with symmetric links this is a reasonable approximation
   but is an estimate, not a measurement."* Secondary axis only. Do not put it in an abstract.
3. **Snapshot age in server ticks.** Client reports `server_tick_now − server_tick_of_applied_state`.
   Clock-free, dimensionless, excellent for staleness and jitter, useless for absolute latency. Cheap
   — record it alongside (1) always. Given V3, this measure is *especially* informative: it will show
   the client living on 10 Hz fulls, which is itself a finding.

**Defensible precision, stated plainly for the methodology section:**
per-tick intervals **±1 µs**; same-machine cross-process one-way **±1 µs**; cross-machine RTT
**±1 µs at each endpoint's own clock**, with the true one-way split unknown; cross-machine one-way
**not claimed**. If a genuine cross-machine one-way number is ever required, the honest path is PTP
with the measured offset bound reported — not `w32tm`.

**Sim-time vs wall-time.** Every record carries **both** `sim_tick` (a monotonic integer the server
owns) and `t_us`. All cross-process *correlation* — notably E2's oracle diff — keys on `sim_tick` and
is therefore immune to every clock question above. Only latency uses `t_us`.

---

## C. Harness

### C.1 Decision: extract a shared core; build a separate headless CLI runner. Do **not** extend the WPF launcher.

**Justification.**

- **WPF cannot run unattended.** It requires an interactive desktop session — no SSH, no Task
  Scheduler under a service account, no CI. An overnight 6-experiment × 5-repetition matrix is the
  entire point of the exercise.
- **It has no failure semantics.** `MainWindow.OnLaunch` is `async void` and returns nothing. A
  harness must exit non-zero when a role dies so a wrapper can distinguish "cell completed" from
  "cell crashed and left a short CSV". `async void` cannot even propagate an exception.
- **Its bring-up is a race dressed as a constant.** `ManagerToMidwareDelayMs = 1200`,
  `MidwareToClientDelayMs = 1000`, `ClientStaggerMs = 500` (`MainWindow.xaml.cs:27-29`). A harness
  must wait on **observed readiness** with timeouts, or runs silently start mid-bootstrap and the
  warm-up window is meaningless.
- **But `RoleProcess`, `TelemetryParser`, `AgentProtocol`, `RemoteAgentClient`, and `AgentMode` are
  good and already work.** Verbatim stdout forwarding, exit-code reporting (`RoleProcess.cs:67-72`),
  `taskkill /T /F` tree teardown, `[server N]` de-tagging, and an NDJSON control channel to remote
  machines are exactly what the harness needs. Rewriting them in PowerShell or Python would duplicate
  the one part of the tooling that is already correct — including the multi-machine path the plan
  depends on.

**Therefore: extract, don't rewrite.**

```
tools/
  DistributedLauncher.Core/     # NEW net9.0 class library, no WPF reference
    RoleProcess.cs              # moved verbatim
    TelemetryParser.cs          # moved verbatim
    AgentProtocol.cs  AgentMode.cs  RemoteAgentClient.cs
    LaunchProfile.cs            # moved; gains the experiment flags
  DistributedLauncher/          # WPF GUI, now referencing .Core — behaviour unchanged
  ExperimentRunner/             # NEW net9.0 console — the harness
```

`ExperimentRunner` is a .NET console app rather than PowerShell or Python **because it reuses `.Core`
directly**, including the remote-agent client. It remains trivially scriptable
(`dotnet run --project tools/ExperimentRunner -- run plans/exp2-baseline.yaml`) from PowerShell and
from Python's `subprocess`, which is where the analysis layer lives.

The WPF launcher keeps its role: interactive bring-up and the live dashboard, used for demos and for
the non-headless evaluation screenshots. It is not on the measurement path.

### C.2 `ExperimentRunner` structure and the cell state machine

```
tools/ExperimentRunner/
  Program.cs           # verbs: doctor | plan validate | run | resume | collect
  ExperimentPlan.cs    # YAML model: defaults, matrix, seeds, topology, exclusions
  PlanExpander.cs      # cartesian product -> ordered List<Cell>, applies exclusions
  Cell.cs              # one (config, rep); deterministic run_id from a content hash
  CellRunner.cs        # the state machine below
  Readiness.cs         # @@STAT-driven readiness and liveness gates
  HealthMonitor.cs     # crash / stall / drop detection -> RunOutcome
  Topology.cs          # local vs remote-agent placement; CPU affinity assignment
  RunDirectory.cs      # on-disk layout + run.json manifest
  EnvironmentProbe.cs  # CPU, cores, RAM, OS build, QPC freq, git sha+dirty, exe hashes, MSVC toolset
  Collector.cs         # pull MetricSink outputs (local copy / agent transfer)
  Postcheck.cs         # the validity gates in C.4
```

**One cell = one full system bring-up:**

```
PREPARE   create run dir; write run.json (config + env); kill stale EntryPoint.exe processes
LAUNCH    Manager  --autostart --headless --duration ... --warmup ... --run-id ... --metrics-dir ...
          wait: @@STAT role=manager                                   [timeout 10 s]
          local Midware (+ remote midwares via RemoteAgentClient)
          wait: @@STAT role=manager midwares=<expected>               [timeout 20 s]
          wait: @@STAT role=server id=0..N-1 (all present)            [timeout 30 s]
          Clients, count == plan.clients exactly
          wait: every server reports game=1                           [timeout 30 s]  <- t0
WARMUP    hold plan.warmup_s; roles tag records phase=0 themselves (never trimmed post hoc)
MEASURE   hold plan.duration_s; poll @@STAT at 2 Hz as a heartbeat ONLY
DRAIN     roles hit --duration, flush sinks, exit(0); wait up to 15 s each
TEARDOWN  taskkill /T /F any survivor; RemoteAgentClient.Stop() per agent
COLLECT   copy metrics trees + stdout logs into the run dir
POSTCHECK apply C.4 gates -> outcome = ok | failed | invalid
```

The readiness gate replacing the fixed delays is the single largest reliability win. Note also that
the game only starts once `mClientCount == mClientMax` in `DistributedPacketSenderServer::AddPeer` —
launch one client too few and **every server hangs at `game=0` forever**. `Readiness.cs` converts
that hang into a clean timeout failure.

Three launch details the harness must not inherit from the GUI launcher:

- **Clients must run headless.** `LaunchProfile.BuildClientArgs` deliberately omits `--headless` so
  the operator can watch them; an unattended matrix must add it, or every cell opens N OpenGL windows
  and competes with the servers for GPU and scheduler time.
- **One game instance per manager process** (H7): `GameInstance::CalculatePhysicsServerBorders` has an
  off-by-`serverIDBuffer` loop bound that starves a second instance of borders. The state machine
  already tears the manager down per cell — keep it that way and record why.
- **Server ports are derived, not configured**: `mPacketSenderServerPort = peerID * 10 + 1000`
  (`DistributedGameServerManager.cpp:66`). The harness's stale-process sweep in `PREPARE` must
  therefore also confirm those ports are free, or a leftover process from a crashed cell will silently
  absorb connections meant for the new one.

**Warm-up is tagged, not trimmed.** The role receives `--warmup` and stamps `phase` on each record.
Post-hoc trimming by wall-clock in pandas is fragile (it depends on when the harness thinks t0 was);
in-process tagging is exact and self-documenting in the dataset.

### C.3 Failure detection — a crashed role must fail the run

All of these are checked; any one is sufficient:

1. **Process exit before `DRAIN`.** Every role's loop is `while (true)` today, so *any* early exit is
   abnormal; `RoleProcess.Exited` already reports the code (`0xC0000005` access violation,
   `0xC0000409` stack buffer overrun, `3` unhandled C++ exception). Recorded verbatim ⇒ `failed`.
2. **Non-zero exit at `DRAIN`.** Once `--duration` exists, a clean run exits `0`.
3. **Telemetry stall** — no `@@STAT` from a role for > 5 s ⇒ hung ⇒ `failed`.
4. **Any `LAUNCH` timeout** ⇒ `failed`.
5. **Remote agent connection dropped mid-cell** ⇒ `failed`.
6. **Missing or short metric files at `COLLECT`** — expected streams absent, or row counts below the
   floor in C.4 ⇒ `invalid`.

The outcome is written to `run.json` **and encoded in the run directory name**, and
`analysis/load.py` **refuses** to load any run whose `outcome != "ok"` unless `--include-failed` is
passed. This is the structural guarantee that a crashed role can never silently contribute a short
CSV to a figure. Failed cells retry up to `plan.retries` times with a **fresh run_id** (never
overwriting); a cell that exhausts retries marks the matrix incomplete and `ExperimentRunner` exits
non-zero.

### C.4 Post-run validity gates

A cell is `invalid` — ran to completion, data unusable — if any of:

- `dropped > 0` in any `MetricSink` (the instrument perturbed the measurement).
- Actual measure duration deviates from `plan.duration_s` by > 2 %.
- Tick count < 90 % of `duration_s × expected_hz`.
- **Any server reports `real_hz != plan.fixed_hz` at any tick** — adaptive stepping leaked back in (V2).
- **Any two servers report different `real_hz`** in the same cell — cross-server determinism broken (V2).
- **`integrated_objects != owned_objects` on any server** — the V1 fix regressed and every server is
  again integrating the whole world. *(Before the V1 fix, this gate is the detector that proves the
  defect is present; after, it is the regression test.)*
- **Inter-server snapshot ingress > 0** — the V6 cross-subscription regressed.
- **Delta acceptance rate == 0** on any cell whose `snapshot_mode` is `delta` or `adaptive` — V3
  regressed and the deltas are dead again.
- Total handoffs == 0 on a multi-server cell — the workload never reached a border (the exact failure
  mode B1 causes; guard against its return).
- `rss_bytes` grew > 20 % across the measure window — leak regression (B5/V5).
- Any server's owned-object count is 0 for > 1 s — partition collapse.
- Determinism gate (experiment-4 plans only): repeat-run traces are not bit-identical.
- **Delta-to-full fallback rate > 0** — `WritePacket` silently substituted full packets for deltas
  (`NetworkObject.cpp:429-431`), which changes bandwidth by 2.5× without any error surfacing.
- **Handoff ping-pong**: any object whose consecutive handoffs alternate between the same two servers
  within a few ticks — the H2 edge-flapping artefact inflating the handoff-rate axis.
- **Objects mapping to server `-1`** for more than a tick — an H1 border gap.

Two of these are *pre-flight*, not post-run, and belong in `plan validate` so a bad cell never runs:

- **World-extent divisibility (H1).** For each `servers` value in the matrix, reject the cell unless
  each axis span is divisible by `numCols = ceil(sqrt(servers))` and `numRows = ceil(servers/numCols)`.
  Silent int truncation of region borders is a correctness bug that would surface as unexplained
  handoff failures hours into a run.
- **Broadphase extent (H3).** Reject any `world` whose coordinates exceed the hardcoded quadtree
  extent, unless the accompanying C++ change to size the tree from `--world` has landed.

Every gate maps to a defect in §0. That is deliberate: **the harness encodes the audit**, so a
regression that would silently corrupt a figure fails a run instead.

### C.5 Multi-machine, CPU isolation, and Windows-only reproducibility

**Topology.** `Topology.cs` maps midwares to hosts. The controller host always runs the manager
(port 1234 is the rendezvous) and optionally a midware. Remote hosts run `deploy/run-agent.bat`,
already produced by `build-deploy.ps1:118-134` (`DistributedLauncher.exe --agent --port 5099`).

Two small `AgentProtocol` additions are needed:
- `AgentCommand.ExtraArgs`, because `AgentMode.StartMidware` currently **hardcodes** the midware
  argument string (`AgentMode.cs:112`) and cannot pass `--run-id / --metrics-dir / --duration /
  --seed` through.
- A `collect` command returning the agent's metric files (or, less code, have the agent write to an
  agreed UNC path and let the controller copy).

`EnvironmentProbe` must run **per host**, because a scaling curve measured across differently-specced
machines is a confound that must be eliminated or disclosed.

**Recommendation: run experiments 1–5 entirely on one machine.** It removes network jitter, removes
the cross-machine clock question entirely (§B.4), and makes E1's `Δt_handoff` legitimate. Multi-machine
is a supplementary result and the deployment-realism story, not the source of the headline numbers.

**CPU isolation is mandatory for experiment 1.** Eight server processes on one box contend; without
pinning, the scaling curve measures the Windows scheduler as much as the system. `CellRunner` sets
`Process.ProcessorAffinity` and `PriorityClass` from `plan.affinity`, pinning each server to distinct
**physical** cores (SMT siblings excluded), reserving cores for the harness and the clients, and
recording the full mapping in `run.json`. Cells whose server count exceeds the available pinned cores
are excluded from the plan explicitly rather than silently oversubscribed — that exclusion, with its
reason, belongs in the paper.

**Windows/MSVC-only — and what to do about it.** The artefact builds only under MSVC x64 with CMake +
Visual Studio 17 2022; the launcher is WPF/net9.0-windows; `Profiler` includes `<Windows.h>` directly;
the harness will use `taskkill` and `ProcessorAffinity`. There is no Linux path and none should be
invented for this paper. For artifact evaluation this must be handled openly, not hidden:

1. **Default reviewer path is single-machine, multi-process on Windows 11 x64** — one box, no network
   setup, no second machine. This is also the *scientifically preferred* configuration, so the
   convenient path and the correct path coincide. Say so.
2. **Ship a prebuilt `deploy/` bundle** (Release, SHA-256 listed in `run.json`) so a reviewer without
   VS 2022 Build Tools can run the smoke profile without building.
3. **Ship `-Profile figures-only`**, which regenerates every figure from the published dataset with
   **no build, no run, and no Windows requirement** — the analysis layer is pure Python and runs
   anywhere. Most reviewers will use this; it must be flawless.
4. **State the hardware and OS requirement in `README` and `docs/EVALUATION.md`** with the exact
   toolset version (`_MSC_FULL_VER`), and provide `doctor` as a one-command prerequisite check.
5. Optionally provide a Windows GitHub Actions (or self-hosted Windows runner) workflow executing
   `-Profile smoke`, which doubles as the project's only regression test — there is no test suite
   today, so this is worth having independently of the paper.

---

## D. Workload generation

Recall B1: today nothing moves except under human WASD input, and the world is **ballistic only** —
gravity plus initial placement, no motive force of any kind. Objects fall, settle, and stop. There is
therefore no handoff traffic to measure, and **handoff rate — the interesting independent variable —
does not exist as a controllable quantity at all.**

### D.1 `WorkloadDriver`

```
CSC8503CoreClasses/DistributedSystemCommonFiles/WorkloadDriver.{h,cpp}
```

Called from `ServerWorldManager::Update`, before `mPhysics->Update`, in place of the current
`TestObject::Update` input polling. Per object it applies a **force or a velocity** — never a
teleport, because teleporting would bypass the border-crossing logic being measured.

**Determinism is the design constraint, not an afterthought.** Per-object `std::mt19937` seeded as
`hash(run_seed, object_network_id)` — a **per-object stream, never a shared one**. This gives the
property everything else depends on: an object's trajectory is a function of
`(run_seed, its own network id, sim_tick)` **only**, and is therefore identical regardless of which
server owns it, how many servers exist, or in what order objects are updated within a tick. Without
this, E2's oracle comparison (§A.3) is not merely noisy — it is meaningless. All motion is a pure
function of `sim_tick`, never of wall-clock `dt`, for the same reason.

The existing `rand() % 2` cube/sphere choice at `ServerWorldManager.cpp:237` is *accidentally*
deterministic — there is no `srand` anywhere in project code, so the sequence is the default seed 1
and is identical on every server. Replace it with an explicit seeded `std::mt19937` regardless:
depending on an unspecified default seed is not a reproducibility story a reviewer will accept, and
the shape mix (cube vs sphere ⇒ AABB vs sphere collision volume) is itself an experimental parameter
that should be controllable.

### D.2 Models — handoff rate as a first-class independent variable

| Model | Parameters | Handoff rate | Purpose |
|---|---|---|---|
| `static` | — | 0 | Current behaviour. **Control arm** — isolates baseline physics cost with zero handoff traffic. Essential for attributing cost. |
| `random_waypoint` | `speed`, `dwell_ticks`, `bounds` | *emergent*, uncontrolled | Classic mobility model; reviewers recognise it immediately. Objects pick a uniform target, steer to it, pick another. Use as the realism arm — **not** as the primary model for exp 4, precisely because the crossing rate is not controllable. |
| `orbit` | `radius`, `angular_speed`, `centre` (on a border) | **closed form**: `2·ω/2π` crossings/s per object | **Primary model for experiment 4.** Deterministic, collision-free if radii and Y-bands are staggered, and the crossing rate is an exact analytic function of a config parameter — so "error vs. crossing rate" becomes a real curve. |
| `border_shuttle` | `crossing_rate_hz`, `amplitude` | **prescribed directly** | Objects oscillate perpendicular to the nearest border at a commanded rate. The saturation instrument: drive handoff rate to the point where the system breaks and report where that is. This is the strongest single figure available for experiment 1's stress arm. |
| `hotspot` | `n_clusters`, `sigma`, `drift_speed` | emergent, bursty | Gaussian clusters drifting across the world ⇒ **load imbalance**, the realistic failure mode of *static* spatial partitioning and the direct bridge to the plan's "dynamic repartitioning" future work. Excellent discussion-section figure. |
| `brownian` | `impulse_sigma` | low, diffuse | Cheap background motion for experiment 3's object sweep, where the point is object count, not crossings. |

**Collision policy is an orthogonal flag** (`--collisions on|off`), because exp 4's E2 metric requires
it **off** (§A.3) while exps 1/3 want it **on** (broad/narrow phase is a large share of the cost being
measured). Report both arms; do not conflate them.

**Total handoff rate is then `objects × per-object-rate`**, and the plan sweeps it directly rather
than hoping geometry produces it. Record both the **nominal** rate (from config) and the **measured**
rate (from the handoff stream) in every run; a systematic gap between them is itself a finding and
also a bug detector.

### D.3 Specification and injection

Workload parameters are **manager-side configuration flowing to servers over the existing bootstrap
path**. Do not add a second configuration channel.

- Manager gains `--workload <model> --workload-params k=v,k=v --seed N --objects N` and validates them.
- They ride to game servers inside `StartDistributedGameServerPacket`, which already carries
  `objectsPerPlayer` and the per-server border string. Add a **`char workloadSpec[256]`** field — the
  same fixed-array pattern as the existing `borderStr[256]`, which is required because these structs
  cross the wire by raw `memcpy`. **Do not add `std::string` fields**: the existing `std::string`
  members in these packet structs are already a latent wire-format bug (a `memcpy`'d `std::string`
  carries a heap pointer across a process boundary), and adding more would compound it.
- `ServerWorldManager` constructs its `WorkloadDriver` from that spec at instance start.
- The **same seed and spec are written into every `run.json`**, so a run is fully described by
  `(git_sha, plan_cell, seed)` and nothing else.

### D.4 Object placement

Replace the 10×10 grid (B2) with a placement function parameterised by `objects_total` and world
bounds:

- `grid` — `ceil(sqrt(n))` columns × `ceil(n / cols)` rows, **spacing derived from the bounds so
  density stays constant as `n` sweeps**. Without this, experiment 3 confounds object count with
  object density and hence with collision rate — the sweep would measure two things at once.
- `uniform_random` — seeded; the natural companion to the mobility models.
- `border_band` — objects placed within ±`w` of region borders, for maximum handoff pressure per object.

`CreatePlayerObjects` additionally has a latent bug — `startPos.x` is assigned only for `i == 0` and
`i == 1` (`ServerWorldManager.cpp:126-141`), so a third player's grid lands at the origin. Replace the
whole function rather than patching it.

Note the world is `−150..+150` on X/Z by default (`DistributedPhysicsServerDto.cpp:6-10`) and the
per-server borders are stored as **integers** (`PhyscisServerBorderData`), so region edges are
quantised to whole world units. That is harmless for the physics but must be accounted for when
placing a `border_band` workload, and it should be mentioned where the paper describes the partitioning.

---

## E. Data schema and analysis

### E.1 Directory layout

```
experiments/
  plans/
    exp1-scaling.yaml  exp2-baseline.yaml  exp3-object-sweep.yaml
    exp4-handoff.yaml  exp5-snapshot.yaml  exp6-latency.yaml
  results/
    <plan>/
      <run_id>/                  # run_id = <plan>-<cellhash>-r<rep>-<utcstamp>
        run.json                 # manifest: config + environment + outcome + counters
        raw/
          manager/   stdout.log resource.csv
          midware0/  stdout.log resource.csv
          server0/   ticks.csv snapshots.csv handoffs.csv resource.csv trace.csv stdout.log
          server1/   ...
          client0/   latency.csv resource.csv stdout.log
      _index.parquet             # every run.json flattened, one row per run
      _ticks.parquet             # all ticks.csv concatenated, config columns joined in
      _handoffs.parquet  _snapshots.parquet  _latency.parquet
  figures/
    fig1_scaling.pdf / .png ...
    figure_data/fig1_scaling.csv     # the exact plotted numbers
```

Raw CSV is what the C++ writes: self-describing, greppable, and the right thing to publish for the
datasets track. Parquet is a **derived, regenerable cache** built by `analysis/build.py` — never
hand-edited, gitignored.

### E.2 Schemas

`run_id` and every config column live in `run.json` and are **joined in at load time**, keeping the
hot path free of string writes and the files small.

**`run.json`**

```json
{
  "run_id": "exp1-scaling-9f2c1a-r03-20260901T221503Z",
  "plan": "exp1-scaling", "cell_hash": "9f2c1a", "rep": 3, "outcome": "ok",
  "config": {
    "servers": 4, "clients": 1, "objects_total": 2000, "objects_per_server": 500,
    "world": [-150, 150, -150, 150],
    "workload": "orbit", "workload_params": {"radius": 40, "angular_speed": 0.8},
    "seed": 20260901, "collisions": true,
    "snapshot_mode": "adaptive", "snapshot_hz": 60, "full_every": 6,
    "sim_hz": 120, "fixed_hz": true, "predict_horizon_s": 0.1,
    "warmup_s": 20, "duration_s": 300, "trace_stride": 6
  },
  "env": {
    "git_sha": "edd16f3", "git_dirty": false, "build_config": "Release",
    "msc_full_ver": "193933523",
    "exe_sha256": {"Manager": "...", "DistributedPhysicsServer": "..."},
    "hosts": [{"name": "PHYS-01", "cpu": "...", "physical_cores": 16, "logical_cores": 32,
               "ram_gb": 64, "os": "Windows 11 Pro 26200", "qpc_freq": 10000000,
               "qpc_utc_anchor": ["...", 123456789],
               "affinity": {"server0": [0,1], "server1": [2,3]}}]
  },
  "timing": {"t0_utc": "...", "measure_start_us": 20000123, "measure_end_us": 320000456},
  "counters": {"dropped_records": 0, "role_exit_codes": {"server0": 0},
               "delta_written": 90000, "delta_accepted": 89997,
               "interserver_snapshot_bytes_in": 0, "handoffs_total": 4821}
}
```

The three `counters` in bold-relief — `delta_accepted`, `interserver_snapshot_bytes_in`, and the
`integrated_objects` gate in `ticks.csv` — are the machine-checkable evidence that V3, V6, and V1 are
fixed **in the run that produced the figure**. They belong in the published dataset, not just in the
harness.

**`ticks.csv`** — one row per server tick (the workhorse):

| column | type | note |
|---|---|---|
| `sim_tick` | u64 | monotonic, server-owned; the cross-process correlation key |
| `t_us` | u64 | µs since role start (QPC) |
| `phase` | u8 | 0 = warmup, 1 = measure — set by the role |
| `tick_us` | u32 | whole outer tick wall time |
| `phys_us` | u32 | `PhysicsSystem::Update` |
| `world_us` | u32 | `GameWorld::UpdateWorld` — **note: times an empty function today (§A.1); keep the column, expect zeros, say so in `DATASET.md`** |
| `predict_us` | u32 | `PredictFuturePositions` |
| `border_us` | u32 | `CheckPositionOutOfServerBoundries` |
| `substeps` | u16 | `iteratorCount` |
| `real_hz` | u16 | the V2 detector |
| `owned_objects` | u32 | objects this server is authoritative for |
| `integrated_objects` | u32 | objects actually passed through `IntegrateAccel` — the V1 detector |
| `border_objects` | u32 | objects inside the border band |

**`snapshots.csv`** — one row per broadcast:
`sim_tick, t_us, phase, mode(full|delta), objects_written, packets, app_bytes, wire_bytes, build_us`

**`handoffs.csv`** — one row per handoff **from each end**, joined on `(object_id, handoff_seq)`:
`sim_tick, t_us, phase, end(sender|receiver), object_id, src_server, dst_server, handoff_seq,
pos_auth_{x,y,z}, pos_pred_{x,y,z}, vel_{x,y,z}, pos_resume_{x,y,z}, dt_handoff_us, err_e1,
ack_us, status(ok|timeout|duplicate)`

**`latency.csv`** (server-side, from the echo):
`sim_tick, t_us, phase, client_peer, snapshot_tick, rtt_us, snapshot_age_ticks`

**`resource.csv`** (all roles, 2 Hz):
`t_us, phase, rss_bytes, private_bytes, cpu_user_us, cpu_kernel_us, threads, handles`

**`trace.csv`** (opt-in, experiment 4 E2):
`sim_tick, object_id, owner_server, x, y, z` — float32 suffices and halves the size.

Every file is tidy/long-form, so analysis is literally
`df.groupby(["servers","objects_per_server"])["tick_us"].quantile([.5,.95,.99])`.

### E.3 Analysis layer

```
analysis/
  pyproject.toml      # pinned: pandas, pyarrow, numpy, matplotlib, scipy (+ committed lockfile)
  build.py            # raw CSV + run.json -> parquet (idempotent, cached)
  load.py             # load_runs(plan, include_failed=False) -> DataFrame
  style.py            # one rcParams: serif, 8 pt, vector PDF, colour-blind-safe cycle
  stats.py            # ci95, bootstrap CIs, pooled quantiles, Mann-Whitney
  figures/  fig1_scaling.py ... fig6_latency.py
  run_all.py          # build + every figure + every figure_data CSV
```

**Conventions, applied uniformly in `style.py`:**

- **Repetitions are the error bar.** Compute the per-run statistic first (e.g. p95 of `tick_us` within
  one run), then plot mean ± 95 % CI **across the N reps**. Never pool raw ticks across reps for a
  central-tendency plot — that hides run-to-run variance, which is the variance a reviewer cares about.
- **Distributions get CDFs, not bar charts.** Experiments 4 and 6 are CDFs (`np.sort` + `linspace`),
  with the boxplot the plan asks for as an inset on fig 4. Pool raw samples across reps for the CDF
  but overlay faint per-rep curves so run-to-run stability is visible.
- **Log axes** where the data spans decades: object count (fig 3, log-x), handoff error (fig 4, log-x),
  latency (fig 6, log-x), bandwidth (log-y where full/delta differ by >10×).
- **The baseline is a horizontal reference line on every relevant figure**, exactly as the plan
  demands ("Baseline çizgisi tüm grafiklerde"). `fig2` exports its value to
  `figure_data/baseline.json`; every other figure imports it. This is a hard dependency and is why
  experiment 2 must run first.
- **Experiment 1 gets an ideal-scaling reference line** (throughput ∝ N). The gap between measured and
  ideal *is* the result.
- Every figure writes `figures/figure_data/<name>.csv` containing exactly the plotted points — a soft
  requirement for artifact evaluation and the thing that makes the paper's numbers checkable.

**Figure map:**

| Figure | Exp | Form |
|---|---|---|
| `fig1_scaling` | 1 + 2 | x = servers (1…8); left y = p50/p95/p99 `tick_us` with CI ribbons; right y = aggregate object-substeps/s vs. the ideal line; baseline hline. **Second panel: `integrated_objects / owned_objects` per server** — the V1 evidence, and a compelling before/after if the pre-fix data is shown. |
| `fig2_baseline` | 2 | tick-time CDF at N=1 across object counts; the reference every other figure cites |
| `fig3_objects` | 3 | stacked panels sharing log-x objects/server: (a) p50/p95/p99 tick time, (b) bytes/s and packets/s |
| `fig4_handoff` | 4 | main: position-error CDF (log-x), one curve per server count; inset: boxplot by crossing rate; panel 2: E2 divergence vs. ticks-since-handoff |
| `fig4b_horizon` | 4 bonus | error p50/p95 vs. `--predict-horizon` — a curve the thesis did not have |
| `fig5_snapshot` | 5 | grouped bars: bytes/object/s and packets/s for `full` vs `delta` vs `adaptive` across object counts, **annotated with the 1-unit delta quantisation error and with the delta-acceptance rate** so the fidelity cost is impossible to overlook |
| `fig6_latency` | 6 | RTT CDF per server count; secondary panel: snapshot-age-in-ticks CDF (the clock-free measure) |

### E.4 Plan file format (YAML, round-trips to the JSON the C++ and `LaunchProfile` already speak)

```yaml
# experiments/plans/exp1-scaling.yaml
plan: exp1-scaling
description: "Scaling curve at constant total object count (Experiment 1, with the exp-2 baseline arm)"
defaults:
  deploy: ../../deploy
  build_config: Release
  clients: 1
  # World extents are constrained, not free (see H1/H3):
  #  - each axis span must be divisible by numCols = ceil(sqrt(servers)) and
  #    numRows = ceil(servers/numCols), or int-truncated borders leave gaps/overlaps;
  #    300 is divisible by 1, 2 and 3, which covers servers in {1,2,3,4,6,8}.
  #  - the broadphase quadtree is hardcoded to 256 half-extent, so |coordinate| <= 256.
  # Scale the experiment by object COUNT at constant density, never by world size.
  world: [-150, 150, -150, 150]
  workload: orbit
  workload_params: { radius: 40.0, angular_speed: 0.8, y_band: 4.0 }
  collisions: true
  sim_hz: 120
  fixed_hz: true            # pin realDT; adaptive stepping is a confound (V2)
  snapshot_mode: adaptive   # 1 full : 5 delta at 60 Hz  == 10 Hz full + 50 Hz delta (V4)
  predict_horizon_s: 0.1
  warmup_s: 20
  duration_s: 300
  repetitions: 5
  retries: 2
matrix:
  servers:       [1, 2, 3, 4, 6, 8]
  objects_total: [2000]     # held CONSTANT -> objects/server falls as servers rise
seeds: [20260901, 20260902, 20260903, 20260904, 20260905]
topology:
  manager: local
  midwares: [{ host: local, servers: all }]
  affinity: { strategy: distinct-physical-cores, reserve_for_harness: [0] }
output: { root: ../results/exp1-scaling }
```

Experiment 4 differs most: `workload: orbit`, `collisions: false` (required for E2),
`trace: true`, `predict_horizon_s: [0.05, 0.1, 0.2, 0.4]`, `workload_params` swept over
`angular_speed` to sweep crossing rate, and an `oracle: { baseline_plan: exp2-baseline }` key so the
analysis knows which N=1 traces are the ground truth for the same seeds.

---

## F. Reproducibility

Aimed at the MMSys OSS & Datasets track, which judges the artefact itself.

**One command.**

```powershell
powershell -ExecutionPolicy Bypass -File tools\reproduce.ps1 -Profile full
```

which runs `doctor` (prerequisites), `build-deploy.ps1 -Config Release`, `EnvironmentProbe`, every
plan in `experiments/plans/`, then `python analysis/run_all.py`.

Three profiles, and the ordering of importance is the reverse of the obvious one:

- `-Profile figures-only` — regenerates every figure from the **published dataset**, no build, no run,
  no Windows. **This is what most reviewers will run, so it must be flawless.**
- `-Profile smoke` — ~10 minutes: 1 repetition, 30 s cells, servers ∈ {1, 2}. Validates the pipeline
  end-to-end before a reviewer commits a night to it. Doubles as the project's only regression test.
- `-Profile full` — the real matrix. Hours.

**Pinned configuration.** Plans are checked in and content-hashed into `cell_hash`. Python
dependencies pinned by exact version with a committed lockfile. ENet and Recast are already vendored
in-tree. **Pin the MSVC toolset version** (`_MSC_FULL_VER`) in `run.json`: floating-point codegen
differences between toolsets will move the E2 divergence numbers, and a reviewer on a different
toolset must be able to see that that is why.

**Environment metadata, captured automatically per run** — CPU model, physical/logical core counts,
RAM, OS build, `QueryPerformanceFrequency` and a QPC↔UTC anchor, git SHA **and dirty flag**, build
configuration, SHA-256 of every deployed `EntryPoint.exe`, MSVC toolset, and the CPU affinity map.
A dirty working tree sets `git_dirty: true`, and `ExperimentRunner` **warns loudly and refuses
`-Profile full`** unless `--allow-dirty` — a published number produced from uncommitted code is
unreproducible by construction.

**Dataset publishing.** The raw tree (parquet + `run.json` + plans + `figure_data`) goes to Zenodo
with a DOI, cited from the paper and the README. Sizing: `ticks.csv` at 120 Hz × 300 s × 56 B ≈ 2 MB
per server-run; the full 6-experiment matrix at 5 reps is single-digit GB raw, low hundreds of MB as
parquet+zstd. `trace.csv` dominates — publish it for the experiment-4 cells only. Ship `DATASET.md`
with the §E.2 schema tables verbatim (including the honest note that `world_us` times an empty
function) and a `load_example.ipynb` reproducing `fig1` in ten lines.

**Also required for a credible artefact:** `docs/EVALUATION.md` covering the methodology and every
caveat this document raises — warm-up rationale, why collisions are disabled for experiment 4, the
RTT/2 caveat, the delta 1-byte quantisation cost, the absence of interest management (V7), and the
single-machine rationale. Correct `docs/NETWORKING.md` (V4, V5) before it is quoted into the paper's
system-design section.

---

## G. Ordered increments

Ordered so that **experiment 2 (the single-server baseline) runs end-to-end soonest** — it is both
the earliest defensible figure and a hard dependency of every other figure's reference line. Each
increment leaves the system working.

### Increment 0 — harness skeleton, zero C++ changes *(≈1 day)*
- Extract `DistributedLauncher.Core`; the WPF launcher references it and still works (regression check).
- `ExperimentRunner` with `plan validate`, `doctor`, and `run`: `CellRunner` doing LAUNCH → readiness
  gates → fixed hold → `taskkill` teardown, driven only by today's `@@STAT`.
- `RunDirectory` + `run.json` + `EnvironmentProbe`; crash detection via `RoleProcess.Exited`.
- `plan validate` enforces the two pre-flight gates: world-extent divisibility (H1) and broadphase
  extent (H3). Cheap, and they prevent a whole class of silent corruption.
- `build-deploy.ps1` default flips to `-Config Release` (B4).

**Deliverable:** a 1-server run launches, holds 60 s, tears down cleanly, and yields a `run.json` with
full environment metadata. Nothing publishable — but readiness gating and failure semantics are proven
before any engine surgery, so every later increment is debugged against a harness that already works.

### Increment 1 — measurement substrate + the two determinism fixes *(≈2 days)* → **experiment 2 becomes real**
- `RunClock`, `ExperimentConfig` (`--run-id --metrics-dir --duration --warmup --seed`), `MetricSink`,
  `Histogram`.
- `HeadlessRunner`: fixed-rate loop, duration-bounded, `onShutdown` flush, `exit(0)` (B3).
- `ticks.csv` from `ServerWorldManager::Update` + the `ServerStarter` tick lambda; `resource.csv` from
  new numeric `Profiler` getters in all four roles.
- **`PhysicsSystem`: `realHZ`/`realDT` become members; `--fixed-hz`; expose `GetRealHZ()` and
  `GetLastSubstepCount()`** (V2).
- **Bound `stateHistory`** (V5) — otherwise the baseline itself drifts with duration and the reference
  line every other figure cites is wrong. (The linear scan in `GetNetworkState` is O(1) *today*; it
  only becomes a cost once V3 lands, so the bound must be in place before Increment 4 — see §0.2 V5.)
- Fix the broadcast packet leak (B5) and the four lifetime/null-check bugs (H6) — an unattended matrix
  exercises these thousands of times and would otherwise produce crashes that look like load.
- `analysis/build.py`, `load.py`, `style.py`, `fig2_baseline.py`.

**Deliverable: experiment 2 runs unattended and produces a publication-quality tick-time CDF with
error bars over 5 repetitions, plus `figure_data/baseline.json` that every later figure consumes.**
This is the earliest point at which the paper has a real number.

### Increment 2 — workload and object count *(≈2 days)* → **experiment 3 at N=1; handoffs begin to exist**
- `WorkloadDriver` with `static`, `orbit`, `random_waypoint`; per-object seeded RNG (B1).
- Placement rewrite: 10×10 cap removed, `--objects` honoured to 1000+, constant-density grid, the
  `startPos.x` bug fixed (B2).
- Workload spec plumbed manager → `StartDistributedGameServerPacket` (`char workloadSpec[256]`) →
  `ServerWorldManager`.
- `fig3_objects.py` (tick-time panel only, N=1).

**Deliverable: experiment 3's object sweep at N=1 — a valid figure, because V1/V6 are inert at N=1.**
Multi-server object sweeps wait for Increment 3.

### Increment 3 — the two scaling-validity fixes *(≈1.5 days)* → **experiment 1 becomes valid**
- **V1: predicate `IntegrateAccel`/`IntegrateVelocity` on `HasPhysics()` / `IsNetworkActive()`** (or
  maintain a separate active-object list). Emit `integrated_objects` alongside `owned_objects` and
  gate on their equality.
- **V6: stop game servers subscribing to peer snapshot streams**; emit `interserver_bytes_in` and gate
  it to zero.
- `fig1_scaling.py` including the ideal-scaling line and the `integrated/owned` evidence panel;
  multi-server arm of `fig3`.

**Deliverable: the scaling curve — the paper's central claim — measured on a system where per-server
cost can actually fall with server count.** Keep one pre-fix run in the dataset: the before/after
contrast is a legitimate and vivid result in its own right.

### Increment 4 — bandwidth and the delta repair *(≈1.5 days)* → **experiment 5**
- Byte/packet counters at `GameServer::SendGlobalPacket`, split by packet type; ENet `totalSentData`
  wire-level cross-check.
- **V3: repair the delta baseline** — `mServerSideLastFullID` driven by a real ack rather than by the
  never-fired input handler; emit `delta_written` / `delta_accepted` and gate acceptance > 0.
- **V4: correct the 20 Hz claim** in code comments and `docs/NETWORKING.md`.
- `--snapshot-mode full|delta|adaptive`; `snapshots.csv`; `fig5_snapshot.py`; bandwidth panel of fig 3.

**Deliverable: a full-vs-delta comparison that measures traffic the client actually applies**, with
the 1-byte quantisation cost reported alongside the saving.

### Increment 5 — handoff, first cut *(≈2 days)* → **experiment 4 (E1)**
- `handoffs.csv` written at both ends; `handoff_seq`; `--predict-horizon` replacing the hardcoded
  `0.1f` at `PhysicsSystem.cpp:226`, and gravity made consistent between prediction and integration (H4).
- Reconcile the interval conventions of `IsObjectInBorder` and `GetObjectServer` (H2), and add the
  ping-pong detector to the post-run gates — edge flapping would otherwise masquerade as a high
  handoff rate.
- Decide H5: either send the handoff in the same pass as detection and implement the ack, or keep
  send-and-release and define handoff latency as *send → resume* with the one-tick bias disclosed.
- Implement (or delete) `HandleTransitionHandshakeReceived` (empty body, `ServerWorldManager.cpp:207-210`)
  and `CalculateIncomingObjectOffsetPosition` (dead, returns a dangling reference, `:298-316`).
- `border_shuttle` workload so crossing rate is a commanded parameter.
- `fig4_handoff.py` (E1 CDF + boxplot), `fig4b_horizon.py`.

**Deliverable: the position-error distribution the plan asks for, over thousands of crossings, as a
function of a controlled crossing rate.**

### Increment 6 — handoff, oracle *(≈2 days)* → **experiment 4 (E2)**
- **Run the determinism gate first**: same seed, same server count, twice ⇒ bit-identical traces. If it
  fails, stop — experiment 4 ships as E1 only, and say so in the paper.
- `trace.csv` + `--trace-stride`; `--collisions off`; `border_band` placement.
- Oracle join in analysis (baseline vs distributed traces on `sim_tick`); divergence-vs-time figure.

### Increment 7 — latency *(≈1.5 days)* → **experiment 6 (optional)**
- `senderTicks` in `FullPacket`; `SnapshotAckPacket` + enum entry; client echo; server-side RTT.
  (Note: this is the same ack mechanism Increment 4 needs for V3 — if Increment 4 built it properly,
  most of this is already done.)
- `latency.csv`, snapshot-age-in-ticks, `fig6_latency.py`.
- Multi-machine arm via the extended agent protocol.

### Increment 8 — artefact *(≈2 days)*
- `tools/reproduce.ps1` with `full` / `smoke` / `figures-only`; smoke wired into CI on a Windows runner.
- `docs/EVALUATION.md`, `DATASET.md`, `load_example.ipynb`; corrected `docs/NETWORKING.md`.
- Prebuilt Release `deploy/` bundle with hashes; Zenodo upload; DOI into README and paper.

**Critical path to a submittable evaluation: 0 → 1 → 2 → 3 → 4 → 5.**
Increments 6 and 7 turn a solid MMVE short paper into a full one. Increment 8 is what wins the OSS &
Datasets track.

---

## H. Summary of what changed from the v1 design

| v1 said | v2 says |
|---|---|
| Adaptive timestep (B2) is a confound to pin with `--fixed-hz` | Still true, **and** it makes cross-server trajectories diverge, so it is a *correctness* prerequisite for experiment 4's oracle metric, not just a tidiness fix |
| `objs` counts "objects this server is simulating" | Wrong. Every server *integrates* every object in the world (V1). The scaling claim is unmeasurable until `IntegrateAccel` gains a predicate. **New Increment 3.** |
| Experiment 5 needs byte counters | Needs byte counters **and** a working delta path — 100 % of deltas are currently discarded by the client (V3). Byte counters alone would produce a confident, wrong figure. |
| Server-side `stateHistory` grows without bound | Grows on **both** ends, and `GetNetworkState` linear-scans it per delta write, so per-tick cost degrades **with run duration** — the contamination is largest in exactly the longest cells. Promoted into Increment 1. |
| Snapshot cadence is 60 Hz (code comment says 20) | Confirmed, and `docs/NETWORKING.md:86` repeats the wrong figure; `:88` also wrongly claims `UpdateMinimumState` prunes. Both must be corrected before the system-design section is written. |
| — | **New:** servers subscribe to each other's snapshot streams (V6), giving O(N²) traffic on the exact axis experiment 1 sweeps. Gated to zero in Increment 3. |
| — | **New:** no interest management (V7). Not a bug to fix for this paper — a limitation to disclose, and the bridge to the future-work section. |
| Increment order 0→1→2→3→4 | Reordered: the scaling-validity fixes (V1, V6) are promoted to their own increment **before** bandwidth, because experiment 1 is the paper's central claim and is currently the least valid of the six. |
| — | **New (H1):** borders are computed as `double` and parsed as `int`, so an indivisible world extent produces gaps and overlaps between regions. Promoted to a **pre-flight plan-validation gate** — this would otherwise surface as unexplained handoff failures hours into an overnight matrix. |
| — | **New (H3):** the broadphase quadtree extent is hardcoded to 256. **Scale experiments by object count at constant density, never by enlarging the world** — the v1 example plans used a ±300 world, which would have placed objects outside the broadphase. Corrected here. |
| — | **New (H5):** the handoff send is one tick later than the border detection, and the ack half of the handshake is entirely inert. "Handoff latency" must be defined as *send → resume*, with the one-tick bias disclosed, until the ack is implemented. |
| — | **New:** `WritePacket` silently falls back to a full packet when a delta write fails, a 2.5× bandwidth swing with no error surfaced. Now a first-class metric and a validity gate for experiment 5. |
| — | **New (H6):** four dangling-reference / missing-null-check sites will produce apparently random crashes across an unattended matrix. Fix before the first overnight run, or the harness will faithfully record noise. |

---

## I. Open questions to settle before implementation begins

1. **Does the V1 fix change the physics, or only the cost?** Excluding inactive objects from
   integration stops handed-off objects from continuing to fall on their former owner. That is
   certainly *correct*, but it changes trajectories, so any pre-fix data is not comparable to
   post-fix data. Decide now whether the paper shows the before/after contrast as a result (worth
   doing) or simply reports post-fix numbers.
2. **Is `handoff latency` defined as send→resume, or is the ack implemented?** H5. The former is
   cheaper and honest; the latter is a better system. This choice changes the experiment-4 schema.
3. **Is the delta path repaired properly (a real ack driving `mServerSideLastFullID`) or minimally
   (server tracks its own last-sent full ID)?** The minimal fix makes deltas apply and unblocks
   experiment 5, but leaves no ack for `UpdateMinimumState`, so V5 must then be solved by a ring
   buffer instead. The proper fix solves V3, V5 and experiment 6 at once and is recommended.
4. **How many repetitions, and how long?** The design assumes 5 × 300 s. Confirm against the total
   matrix size and available machine-hours before freezing the plans, because `repetitions` is what
   the error bars are computed over and changing it later invalidates published figures.
5. **Which single machine is the reference machine?** Every headline number should come from one
   documented host. Record it in the paper, not just in `run.json`.

---

## Implementation notes — evaluation layer (2026-08-17)

### What shipped

| Tool | Role |
|---|---|
| `tools/measure.ps1` | One run. Bounded, reproducible mode, per-run manifest. |
| `tools/run-experiments.ps1` | One experiment: a sweep over `servers`/`objects`/`ticks`, N repeats per point, one directory per repeat, one manifest per experiment. |
| `tools/analyse.py` | Reads the per-tick CSVs and `@@FINAL` totals, checks every invariant, writes `summary.csv`, prints a table. Exits non-zero if any invariant fails. |

**Repeats are mandatory, not optional.** §17 of the interactions design records that per-server
object counts and handoff event counts vary by ±1 at the same seed, and that closing that needs a
global tick barrier. Performance figures therefore come from repeated runs, and the analysis
reports the **median across repeats** so one slow run cannot drag a point.

Three deliberate choices in the analysis, each correcting a way a naive script would mislead:

- **Percentiles, not means.** Realtime tick cost is bimodal (mean 22x median), so a mean alone
  misrepresents it.
- **Never pool ticks across servers.** Servers carry different loads and tick at different rates, so
  pooling weights whichever ticked more. Per-server stats are reported; cross-server figures are
  sums, never averages of averages.
- **`@@FINAL` only.** `@@STAT` is a 2 Hz sample and is never used for a reported number.
- **Pre-seeded object count is one server's value, not a sum** — every server builds the identical
  set independently, so summing it would multiply the world by the server count.

Standard library only. The dissertation's numbers should not be gated on a `pip install` on the
machine that produces the data.

### First results — 1 and 2 servers, 3 repeats, 3600 ticks, shuttle

> **Debug build — not quotable.** See `2026-08-18-scale-ceiling.md` §0. Relative
> comparisons within this table hold; absolute values are pessimistic by an
> unknown factor.

| servers | server | p50 (ms) | p95 (ms) | p99 (ms) | owned |
|---|---|---|---|---|---|
| 1 | 0 | 1.5511 | 1.9480 | 2.3622 | 400 |
| 2 | 0 | 1.2990 | 1.6061 | 1.9446 | 361 |
| 2 | 1 | 0.0668 | 0.1015 | 0.1265 | 39 |

All invariants exact on every run: conservation, handoff parity, command accounting, `hoFail = 0`,
`hoLate = 0`, no resurrections, and `integrated == owned` on **every** post-warmup tick.

Note what the partition actually did here: the shuttle workload leaves 361 of 400 objects on server
0, so two servers is not a halving of load — it is a 90/10 split. The per-server cost drop
(1.55 → 1.30 ms p50) is proportionally much smaller than the object split, which is the interesting
result and exactly the kind of thing a single pooled average would have hidden.

### The 4-server failure — FIXED, and it was one word

4-server runs failed on **3 of 3** attempts, always losing exactly one server, but not always the
same one (s2, s2, s0). That "exactly one, varying" signature is what pointed at the answer.

**Root cause.** `SystemManager::SendStartGameStatusPacket` used `SendGlobalPacket` — the
**unreliable** ENet variant (packet flag 0) — for `GameStartState`. It is a one-shot bootstrap
message with no retry anywhere in the system: a server that misses it never builds its world, sits
at `game=0` forever, and produces no metrics. Dropped for roughly one recipient in four, which is
precisely the observed failure.

Snapshots correctly use the same unreliable call — they are superseded 60 times a second. A
bootstrap message is not, and the two had been written the same way. One word: `SendGlobalPacket`
to `SendGlobalReliablePacket`.

Two other real defects were found and fixed while narrowing this, and both stay fixed:

- The readiness test was an **exact** `==` evaluated only on peer join, while the expected count
  arrives later via `SetMaxClients`; the equality could be stepped over entirely.
- `ConnectServerToAnotherGameServer` returned a `GameServerConnection` **even when the connect
  failed**, so a peer whose sender server was not yet listening was recorded as connected and its
  handlers never registered. Failures are now reported and retried every 0.5 s.

**Verified: 9/9 runs pass, including 4/4 servers on all three 4-server runs**, every invariant
exact, analyser exit code 0.

### Scaling result — 1, 2 and 4 servers, 3 repeats, 3600 ticks, shuttle

Clean-tree dataset, commit `7a48341`, medians across 3 repeats:

> **Debug build — not quotable.** See `2026-08-18-scale-ceiling.md` §0. Relative
> comparisons within this table hold; absolute values are pessimistic by an
> unknown factor.

| servers | server | p50 (ms) | p95 (ms) | p99 (ms) | owned |
|---|---|---|---|---|---|
| 1 | 0 | 1.548 | 1.820 | 1.977 | 400 |
| 2 | 0 | 1.288 | 1.516 | 1.670 | 362 |
| 2 | 1 | 0.061 | 0.092 | 0.118 | 38 |
| 4 | 0 | 0.077 | 0.111 | 0.205 | 27 |
| 4 | 1 | 0.037 | 0.058 | 0.111 | 5 |
| 4 | 2 | **1.196** | 1.512 | 1.854 | **331** |
| 4 | 3 | 0.088 | 0.132 | 0.254 | 36 |

All invariants exact on all 9 runs; `integrated == owned` on **every** post-warmup tick;
analyser exit code 0.

**The headline is the imbalance, not the speedup.** Static spatial partitioning barely spreads this
workload. At 4 servers one server still holds **331 of 400 objects (83%)**, and the busiest
server's p50 falls only 1.55 → 1.20 ms (**23%**) for a 4x increase in servers. Going 1→2 moved 38
objects; 2→4 moved another 31.

That is a useful negative result and it is the honest motivation for the paper's direction: it
quantifies why a static region partition is insufficient, and sets the bar an adaptive or
load-aware partition has to beat. A pooled cross-server average would have reported a comforting
~0.35 ms at 4 servers and hidden the finding entirely — which is exactly why `analyse.py` refuses
to pool across servers.

**Caveat on the workload.** The shuttle workload launches every object from one region, so this
measures partitioning under a deliberately adversarial distribution. A uniform workload would give
a very different curve. `--workload seam` exists for the border-ownership case; a uniform mode is
the obvious next addition, and it is what would turn this into a fair speedup measurement rather
than a worst-case one.

---

## The balanced counterpart — `--workload uniform` (2026-08-17)

The shuttle result above measures a deliberately adversarial distribution: every object is launched
from one region, so a static partition starts ~90% loaded on one server. That is a worst case, not
a speedup measurement. `--workload uniform` is the counterpart — the same deterministic motion
model, but the starting grid is spread across the whole world, sized from the union of every
server's region (`GetWorldExtent`) so it can never fall outside the partition it is measured
against.

Clean-tree dataset, commit `fddfbf3`, 3 repeats, 3600 ticks, 400 objects, medians:

> **Debug build — not quotable.** See `2026-08-18-scale-ceiling.md` §0. Relative
> comparisons within this table hold; absolute values are pessimistic by an
> unknown factor.

| servers | server | p50 (ms) | p95 (ms) | p99 (ms) | owned |
|---|---|---|---|---|---|
| 1 | 0 | 1.645 | 1.990 | 2.332 | 400 |
| 2 | 0 | 0.617 | 0.785 | 1.006 | 199 |
| 2 | 1 | 0.596 | 0.762 | 0.970 | 201 |
| 4 | 0 | 0.260 | 0.348 | 0.539 | 98 |
| 4 | 1 | 0.273 | 0.366 | 0.556 | 108 |
| 4 | 2 | 0.263 | 0.352 | 0.513 | 101 |
| 4 | 3 | 0.224 | 0.311 | 0.495 | 93 |

All invariants exact on all 9 runs; `integrated == owned` on every post-warmup tick.

### The two workloads bracket the contribution

Busiest-server p50, and the reduction relative to one server:

> **Debug build — not quotable.** See `2026-08-18-scale-ceiling.md` §0. Relative
> comparisons within this table hold; absolute values are pessimistic by an
> unknown factor.

| servers | uniform | | shuttle | |
|---|---|---|---|---|
| 1 | 1.645 ms | 1.00x | 1.548 ms | 1.00x |
| 2 | 0.617 ms | **2.67x** | 1.288 ms | 1.20x |
| 4 | 0.273 ms | **6.03x** | 1.196 ms | 1.29x |

Object spread at 4 servers: uniform **98 / 108 / 101 / 93** (range 15) against shuttle
**27 / 5 / 331 / 36** (range 326).

**The 4-server uniform figure is superlinear — 6.03x on 4 servers.** At one server, 400 objects
cost 1.645 ms; at four, ~100 objects cost 0.273 ms, where a linear cost model predicts 0.411 ms.
Physics cost grows faster than linearly in object count (broadphase plus pairwise resolution), so
quartering the objects on a server more than quarters its tick cost. Note this is the reduction in
**per-server tick cost** — what decides whether a server holds its frame budget — not a wall-clock
throughput speedup; each server is a separate process on its own core.

Reported together these two numbers are the honest statement of what static spatial partitioning
buys: **6x when the world is evenly populated, 1.3x when it is not**, with the partition unable to
respond to the difference. That gap is the case for load-aware or adaptive partitioning, and it is
now measured rather than asserted.

---

## Object-count sweep — why the speedup is superlinear (2026-08-17)

The uniform result reports a **6.03x** reduction in busiest-server tick cost on 4 servers, which is
superlinear. That was explained above by asserting physics cost grows faster than linearly in object
count. This sweep measures it instead of asserting it.

Clean-tree dataset, commit `5ab6880`, 2 servers, uniform workload, 3 repeats, 3600 ticks:

> **Debug build — not quotable.** See `2026-08-18-scale-ceiling.md` §0. Relative
> comparisons within this table hold; absolute values are pessimistic by an
> unknown factor.

| objects (total) | server | owned | p50 (ms) | p95 (ms) | p99 (ms) |
|---|---|---|---|---|---|
| 100 | 0 / 1 | 53 / 47 | 0.101 / 0.092 | 0.135 / 0.122 | 0.212 / 0.207 |
| 400 | 0 / 1 | 200 / 200 | 0.594 / 0.581 | 0.761 / 0.751 | 0.985 / 0.990 |
| 1600 | 0 / 1 | 804 / 796 | 7.147 / 6.594 | 8.215 / 7.552 | 9.428 / 8.874 |

All invariants exact on all 9 runs.

### The cost model

Taking the busiest server at each point and fitting `t ~ n^k`:

> **Debug build — not quotable.** See `2026-08-18-scale-ceiling.md` §0. Relative
> comparisons within this table hold; absolute values are pessimistic by an
> unknown factor.

| objects owned | p50 (ms) | µs per object | n growth | cost growth |
|---|---|---|---|---|
| 53 | 0.1013 | 1.91 | — | — |
| 200 | 0.5940 | 2.97 | 3.77x | **5.86x** |
| 804 | 7.1471 | 8.89 | 4.02x | **12.03x** |

**Fitted exponent: k = 1.57 overall** — and it *rises* with density, 1.33 on the first segment and
**1.79** on the second. Per-object cost nearly quintuples between 53 and 804 objects (1.91 → 8.89
µs).

This is the mechanism behind the superlinear speedup, and it now predicts it quantitatively.
Splitting a 400-object world across N servers gives each server 400/N objects, so the expected
per-server cost reduction is `N^1.57`:

| servers | predicted `N^1.57` | measured (uniform) |
|---|---|---|
| 2 | 2.97x | 2.67x |
| 4 | 8.80x | 6.03x |

Measured falls short of predicted, and the gap widens with N. That is the expected signature of
fixed per-tick overhead that does **not** scale down with object count — the network pump, snapshot
broadcast and border check run once per tick regardless. Partitioning divides the superlinear part
and leaves the constant part alone, so the returns taper.

### Two things worth carrying into the write-up

- **The exponent is drifting toward quadratic.** k = 1.79 on the 400 → 1600 segment suggests the
  broadphase is degrading at high density rather than holding its expected behaviour — plausibly
  quadtree nodes exceeding their split threshold in a fixed 300x300 world, pushing more pairs into
  narrow phase. Worth confirming before quoting k as a property of the *system* rather than of this
  configuration.
- **1600 objects on 2 servers is at the real-time edge.** p50 is 7.15 ms against an 8.33 ms budget
  at the 120 Hz substep rate, and p95 (8.22 ms) is essentially at it. That is a concrete capacity
  statement: this configuration saturates just past 1600 objects, and it is the number a scaling
  argument should be anchored to.
