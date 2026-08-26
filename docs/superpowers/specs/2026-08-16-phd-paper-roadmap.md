# Roadmap: repo → paper → PhD application

Date: 2026-08-16
Status: **plan — no implementation started**
Inputs: the Aug-2026 code audit, `2026-08-16-interactions-toolset-design.md`, `2026-08-16-evaluation-pipeline-design.md`, and a literature sweep (Aura Projection, Dyconits, Kale & Kry 2024, Eibl & Rüde, Chrono::Distributed).
Serves: `PHDFinder/20-PAPER-PLAN.md` (arXiv Nov 2026 → MMVE 11 Jan 2027) and `PHDFinder/00-MASTER-PLAN.md` (application wave Dec 2026 – Mar 2027).

---

## 1. Three findings that reset the plan

### 1.1 The infrastructure is not ready, and the defects point the wrong way

`20-PAPER-PLAN` says *"Altyapı hazır"* — the headless mode, `@@STAT` telemetry and WPF launcher are enough to automate the experiments. They are enough to **launch** a run. They are not enough to **measure** one, and four of the six planned experiments would currently produce plausible-looking numbers that are wrong in a *detectable direction*.

The worst of these is not a measurement bug but a simulation bug: `PhysicsSystem::IntegrateAccel`/`IntegrateVelocity` filter only on `nullptr`, never on `HasPhysics()`/`IsNetworkActive()`. Every server pre-seeds the entire world and merely deactivates out-of-region objects, so **every server integrates every object in the world every substep**. Per-server CPU therefore scales with total world size rather than region occupancy — which is precisely the claim experiment 1 exists to demonstrate. Run today, the scaling curve argues *against* spatial partitioning.

Full defect register: `2026-08-16-evaluation-pipeline-design.md` §0 (validity defects V1–V7, second-order hazards H1–H8).

### 1.2 Predictive handoff is dominated by Aura Projection — from the same institution

**A. Brown, G. Ushaw, G. Morgan, "Aura Projection for Scalable Real-Time Physics", I3D 2019**, DOI [10.1145/3306131.3317021](https://doi.org/10.1145/3306131.3317021).

Two corrections to how AP is commonly characterised, both load-bearing:

- AP is **not** an MMO interest-management paper. It is a distributed **rigid-body physics** paper — the same problem as this project.
- "Projection" does **not** mean projecting the aura forward along the velocity vector. It means projecting the aura **spatially onto the neighbouring server** across a boundary. The aura itself is a symmetric sphere and is a *trigger volume with no physics*.

And the part that matters most: **AP already does velocity-based lookahead migration.** Its aura radius is

```
R_a = R_o + (V_t · T_T)
```

where `V_t` is a speed tolerance and `T_T` a displacement-time bound derived from frame time and latency. The `V_t · T_T` term *is* a predictive lookahead sized to cover latency plus two frames. So "extrapolate ahead and hand off early" was published by Newcastle in 2019.

Worse, AP's version has the **stronger correctness story**. AP uses a conservative global speed tolerance and states its envelope explicitly (*"if velocities, latencies, or frame-time are above these tolerances, then stability is no longer guaranteed"*). Per-object actual-velocity extrapolation is *tighter* — fewer unnecessary migrations — but **unsound**: an object that accelerates or is struck near the boundary violates the prediction, and there is no fallback envelope.

> **Action required.** Check how the dissertation characterises Aura Projection. If it describes AP as directional forward-projection, that is a factual error about the cited direct ancestor. Note also that **Gary Ushaw is a co-author** — the same Gary Ushaw named in `00-MASTER-PLAN` as a referee and Newcastle contact. Getting AP right is both a correctness obligation and a relationship asset.

### 1.3 The obvious "next contribution" was taken in 2024

**M. Kale, P. G. Kry, "Distributed Simulation of Large Multi-body Systems", arXiv:2403.17261 (2024)** — McGill + Huawei Canada — partitions bodies by a constraint graph, places interface bodies in an **overlap set** simulated redundantly by multiple workers, blends the results by graph geodesic distance, and treats inter-worker collisions as load-balancing opportunities. It **cites Aura Projection and positions against it explicitly**.

So "overlap/ghost regions plus load balancing, contrasted with Aura Projection" is already staked. It must be confronted in related work, not discovered by a reviewer.

Their weaknesses are the openings: no real-time deadline, LAN-only, fixed worker count, no clients at all, physically incorrect blending (they say so), and a central main server holding a global view of every body.

---

## 2. Revised contribution claim

The current claim — *"end-to-end system with spatial partitioning, predictive handoff, and delta snapshot replication"* — should be restructured. Predictive handoff cannot carry the paper.

**Demote predictive handoff to a supporting lemma.** Not "we extrapolate" (that is dead reckoning, and a PDES-literate reviewer will additionally note the 100 ms horizon is a **Chandy–Misra–Bryant lookahead** in the exact technical sense). Instead: derive a joint *(lookahead horizon, halo width)* condition under bounded acceleration that guarantees no missed cross-boundary contact within a stated latency envelope, and use it to **size the halo**. That is a correctness claim rather than a mechanism claim, it is genuinely unpublished, and it is defensible.

**Promote dynamic repartitioning to the headline**, if the timeline allows it (see §3, Track 3). It is named as future work by AP (2019), left unsolved by Kale & Kry (2024 — fixed worker count, and they admit their entity-count load metric is inadequate), and untouched by HPC, which optimises batch throughput rather than tail latency. The open problem, stated precisely:

> No published algorithm provides an online repartitioner for spatially-decomposed **real-time** physics that simultaneously (i) drives from *measured* per-region tick time rather than entity count, (ii) bounds per-tick migration work so the rebalance never itself blows the frame deadline, and (iii) stays stable under adversarial player-driven clustering.

This project has an unusual head start on (i): `TelemetryReporter` + the launcher dashboard already carry a measured per-region CPU signal end to end. Most papers in this space must build that from scratch.

**Position Dyconits as complementary, and make the complementarity quantitative.** Dyconits (**Donkervliet, Cuijpers, Iosup — ICDCS 2021**, DOI [10.1109/ICDCS51616.2021.00021](https://doi.org/10.1109/ICDCS51616.2021.00021); *not* Eickhoff, whose work with that group is Meterstick) bounds **observational** inconsistency on the server→client path: staleness and numerical error, per (client, dyconit) pair, enforced optimistically. Handoff error is **simulation** inconsistency on the server→server path: if two servers disagree about ownership during the step in which a body collides, the trajectory is wrong for every observer and no client-side bound can recover it.

The strong move: a ghost/halo layer *costs* server↔server bandwidth; Dyconits *recovers* 49–85% of server→client bandwidth. **The halo can be paid for out of Dyconits' savings.** That is a concrete composition claim, not a hand-wave.

---

## 3. Tracks

Ordering constraint: **fix validity → freeze → measure → then build features.** Any result generated before the fixes must be discarded, so measurement cannot start until Track 1 lands.

### Track 0 — Correctness triage — **VERIFIED ON A RUNNING SYSTEM 2026-08-16**

Implemented, built, deployed and confirmed by a headless 2-server / 1-client run
(`--fixed-step --seed 42 --workload shuttle`, 35 s):

| Claim | Evidence |
|---|---|
| Integrator respects ownership | `server 0: objs=13 integ=13` · `server 1: objs=7 integ=7`. Previously both would have shown `integ=20`. |
| Handoff parity (I5) | `server 0: hoSent=9 hoRecv=2` · `server 1: hoSent=2 hoRecv=9` — each server's sends exactly match the other's receives, `hoFail=0`. |
| Object conservation (I2) | `13 + 7 = 20 = total`. |
| Determinism | Two independent runs at the same seed produced **identical** handoff counts (9/2 and 2/9). |
| Delta path works | Client: `full=6907 dOK=34382 dRej=9`. The 4.98:1 applied-delta-to-full ratio matches the 1 full : 5 delta send cadence. The 9 rejections are the startup transient before the first acknowledgement. |
| Determinism flags reach the servers | `Determinism: fixed-step=on seed=42 workload=shuttle` in each spawned server's log. |

**Before this run the delta path had never once worked**, and the default scene could
not produce a single border crossing.

| Fix | Where |
|---|---|
| Integrator ownership filter | `PhysicsSystem::IntegrateAccel/IntegrateVelocity` now skip `!HasPhysics()`, matching `BroadPhase` |
| Pinned timestep | `realHZ`/`realDT` were file-scope globals shared by every instance in a process; now members `mRealHZ`/`mRealDT` with `SetFixedTimestep`, wired to `--fixed-step` |
| Prediction horizon | The hardcoded `0.1f` is now `mPredictionHorizon` with an accessor; the computed-then-discarded `dt` is documented as deliberate |
| Proper delta ack | New `DistributedClientSnapshotAck` packet; client acks once per pump, server keeps a per-client high-water mark and derives the baseline from the minimum, then prunes history |
| Delta base resolution | `ReadDeltaPacket` resolves its base from `stateHistory` by ID instead of demanding the newest full state — see note below |
| `stateHistory` bound | `TrimStateHistory()` caps at 64 entries regardless of acknowledgements |
| Border truncation | `PhysicsServerBorderData` is `float`, parsed with `stof`; `GetServerAreaString` emits 9 significant digits |
| Pool `.at()` crash | Both handoff sites use a checked `find()` and count the failure |
| 20-peer overflow | `mPeers` allocated at `mClientMax`, reallocated in `SetMaxClients`, freed in the destructor |
| Disconnect leak | The peer-release loop ran to a hardcoded 3, so slots past index 2 leaked and `mClientCount` never fell |
| Deterministic world | `rand() % 2` replaced by a hash of `(seed, playerID, objectIndex)`, wired to `--seed`; independent of call order |
| Handoff parity (I5) | `hoSent`/`hoRecv`/`hoFail` counters through `Profiler` into the `@@STAT` server row |
| Integration validity | New `integ` telemetry key: the count actually integrated per tick, to be compared against `objs` |
| Dangling return | `CalculateIncomingObjectOffsetPosition` returned a reference to a stack local; now by value (still uncalled) |
| Uninitialised flag | `GameObject::mIsNetworkActive` was never initialised in the constructor — indeterminate for any object without a `NetworkObject`, and it gates the handoff border check |

**Note on the delta base.** Encoding deltas against the minimum acknowledged state exposed a second-order bug: `ReadDeltaPacket` accepted a delta only if its `fullID` equalled the client's *newest* full state, so with more than one client every client ahead of the slowest would have rejected every delta — silently reproducing the original defect. The client already retains `stateHistory`, so the base is now resolved by ID.

**Deliberately not done here** (semantic changes, gated on the I5 baseline per §5.1): unifying the two disagreeing border predicates, and the multi-instance border loop bound (`i < mServerCount` mixes an ID offset with a count, so a second game instance gets no borders — single-instance runs are unaffected).

### Track 0 — original scope

From the audit and eval design; each is small and localised.

| Fix | Why it blocks everything |
|---|---|
| Integrator ownership filter (V1) | Otherwise the scaling curve argues against the thesis |
| Pin the timestep (`--fixed-step`) | `realHZ`/`realDT` are runtime-mutated file-scope globals; servers under different load run different steps |
| Delta `fullID` repair **+ bound `stateHistory` together** | Deltas have never applied; fixing one without the other converts a memory leak into a CPU leak |
| Border `double`→`int` truncation (H1) | Produces real **gaps and overlaps** between regions; gap objects map to server `-1` and never hand off |
| `mCreatedObjectPool.at()` → checked `find()` | Currently throws and kills the peer process |
| `GameServer::mPeers` fixed `int[20]` | Heap overflow past 20 peers — **audit which past runs exceeded this; those numbers are void** |
| Handoff-parity counters (invariant I5) | Baseline regression guard, so later protocol changes are attributable |
| Seed the RNG; drop `rand() % 2` | No `srand` anywhere; cross-server agreement is currently accidental |

Then **freeze** and capture the baseline. Keep one pre-fix run in the published dataset — the before/after contrast on `integrated_objects / owned_objects` is a legitimate methodological result.

### Track 0.5 — Build restructure (done 2026-08-16)

`BUILDFORDISTRIBUTEDMANAGER` and `BUILDFORPHYSICSMIDWARE` were global `add_compile_definitions` but are consumed in exactly one file — `main.cpp` — to pick which `ProgramStart.cpp` is `#include`d. Every library was byte-identical across the three server roles, so producing them meant **three full solution rebuilds with a deleted CMake cache**, for three binaries differing by one `#include`.

They are now **per-target** definitions on three `EntryPoint*` executables (`EntryPoint/CMakeDistributedRoles.cmake`). One configure builds Manager + Midware + Game Server together; only the Client needs a second configure, since it is the sole role that flips `DISTRIBUTEDSYSTEMACTIVE`. Four full rebuilds → two configures. The three role executables link in ~31 s off warm libraries.

`build-deploy.ps1` now does two passes instead of four, and `Set-Toggle` takes one argument instead of three — which also removes the failure mode where interrupting the script left `CMakeLists.txt` on a non-default role.

**Docker was considered and rejected.** MSVC requires Windows containers: a 10–20 GB base image, a Windows host with a matching kernel to run it, no use to a reviewer on Linux or macOS, and no help for the OpenGL runtime. It would neither speed up local builds nor serve artifact reproducibility. Pinned VS components plus a `windows-latest` GitHub Actions job (see §Track 1) achieve the reproducibility goal at a fraction of the cost.

> ⚠️ **Measurement runs must use `-Config Release`.** `build-deploy.ps1` defaults to Debug, where MSVC enables iterator debugging and disables inlining — physics runs 10–50× slower and the *shape* of the scaling curve can differ, not merely its scale. Any timing captured in Debug is unusable for the paper.

### Track 1 — Assessment pipeline (Aug–Sep, paper-critical)

Per `2026-08-16-evaluation-pipeline-design.md`. Extract `DistributedLauncher.Core` (keeping `RoleProcess`, `AgentProtocol`, `RemoteAgentClient` verbatim), build a headless `ExperimentRunner` console. WPF cannot run unattended: `async void OnLaunch` has no failure semantics and its fixed 1200/1000/500 ms startup delays are a race dressed as a constant. Readiness gates replace them; ~15 validity gates each map to a specific audit defect, so the harness *encodes* the audit.

Measurement substrate: a per-stream `MetricSink` of POD records, `reserve()`d at construction, overflow = drop-and-count → fail the run, flushed at shutdown. Per-tick data never touches stdout. `@@STAT` demotes to a liveness heartbeat.

Timing: QPC is system-wide consistent across processes on one machine, so same-machine one-way latency is defensible to ~1 µs. Cross-machine uses a server-side echo that never compares clocks; RTT/2 is labelled an estimate.

Scaling caveat: the broadphase quadtree is hardcoded to 256 and `SetNewBroadphaseSize` is never called from the server path — **scale object count at constant density, never world size**.

Increment order puts experiment 2 (baseline) first, because V1/V6/V7 are inert at N=1.

### Track 2 — Interactions toolset (Sep–Oct)

Per `2026-08-16-interactions-toolset-design.md`. Increments 0–5 form a coherent subset that leaves the handoff protocol **completely untouched**: physics registration, ownership unification, command channel, cross-border impulses, runtime spawn.

The literature reframes the priority of this track. Runtime spawn/destroy has **zero novelty** — but AP's benchmark is *160 objects injected per second for 60 s*. **Without runtime spawn the ancestor's experiment cannot be reproduced**, so there is no comparable graph and the evaluation has no anchor to the literature. That makes it the highest evaluation-value-per-hour item on the list, not a nice-to-have.

Prerequisite discovered during design: `PredictFuturePositions` also iterates `mDynamicObjectList`, so without the physics-registration fix a spawned object would never get a predicted position and would **never be handed off** — it would drop out of the predictive protocol silently.

### Track 3 — Ghost/halo + repartitioning (Oct onward, contribution-critical)

**Ghost/halo is table stakes, not a contribution.** "Objects pass through each other at region boundaries" is a desk-reject in a distributed *physics* paper. But ghosting has been standard since Plimpton 1995, ships in Chrono::Distributed, and has two recent real-time instantiations. Implementing it earns the right to be evaluated and nothing more.

The owner rule to copy is Eibl & Rüde's (waLBerla `pe`): one master per body, shadows read-only, **all transformations executed only on the master and synchronised outward**, contact impulses computed against a shadow cached locally and reduced onto the master. That is the canonical answer to double-counting.

Honest constraint to state in the paper rather than paper over: correct rigid-body contact resolution across a boundary is a single LCP split across two processes, requiring velocity exchange **between solver iterations**. At 60 Hz over ENet that is categorically impossible. The defensible escape is the owner-authoritative asymmetric ghost (kinematic on the non-owner), whose cost is that momentum is not conserved across the boundary.

**Repartitioning** is the actual contribution: an ORB/kd-tree split-plane scheme maps almost directly onto the existing rectangular `GameBorder`, giving incremental migration and natural hysteresis. Load metric hierarchy, best to worst: measured per-region tick time > contact count > broadphase pair count > entity count.

---

## 4. Timeline against the application calendar

| When | Track | Gate |
|---|---|---|
| Aug (now) | Track 0 triage → freeze → baseline | Baseline captured; parity counters green |
| Aug–Sep | Track 1 pipeline; experiments 2, 3, then 1 | Experiment 1 valid *only after* V1/V6 land |
| Sep–Oct | Track 2 increments 0–5; experiments 4, 5 | AP-comparable injection workload runs |
| Oct | Draft; PADS 1st round (30 Oct) if ready | Optional — do not let it displace arXiv |
| **Nov** | **arXiv preprint (cs.DC + cs.NI)** | 🔴 Hard: needed on the CV for December |
| Dec–Jan | Applications; Track 3 in parallel | Waterloo 1 Dec, Edinburgh R1, USask/NC State 15 Dec |
| **11 Jan** | **MMVE + MMSys OSS & Datasets** | 🔴 Both, in parallel |

Venue note: the venue sweep rates **DS-RT** as the topical bullseye (8 pp, single-blind, no reframing needed) and **SIGSIM-PADS** as prestige-plus-artifact-badging but requiring genuine PDES-vocabulary reframing. `20-PAPER-PLAN` ranks DS-RT last; it deserves to be higher as the safety net. MMVE remains the right primary target — the sweep under-weighted it by assessing the MMSys research track rather than the workshop.

---

## 4.1 Licensing — checked 2026-08-16, NOT a blocker

A venue sweep raised the engine's provenance as a potential hard blocker on the MMSys OSS & Datasets track, which requires code "licensed in such a manner that it can be legally and freely used (e.g. GPLv2, LGPLv2, BSD, BSD + patents, or equivalent)". Verified against the tree, it is not:

| Component | Licence | Status |
|---|---|---|
| NCL / CSC8503 engine | **MIT**, `LICENSE` at repo root, Copyright (c) 2022 Richard Davison. 57 engine files additionally carry a *"Part of Newcastle University's Game Engineering source code. Use as you see fit!"* header | **Clear.** MIT is more permissive than BSD-3 and unambiguously "equivalent" |
| Recast / Detour / DetourTileCache / DebugUtils | **zlib**, notice in every header (Mikko Mononen) | Clear; notice must be preserved |
| ENet | MIT upstream, but **no licence file is vendored** under `CSC8503CoreClasses/enet/` | Add the upstream `LICENSE` |
| FMOD | Proprietary, non-redistributable | Already behind `#ifndef DISTRIBUTEDSYSTEMACTIVE`; exclude from the released tree |
| `Assets/` | **Undocumented** | The only genuine open question — see below |

**Remaining work (mechanical):**
1. `THIRD_PARTY.md` reproducing the MIT (Davison), zlib (Recast/Detour) and MIT (ENet) notices — an obligation under both licences, not optional.
2. Vendor ENet's `LICENSE`.
3. Add the project author's own copyright line alongside Davison's; keep the whole release under MIT.
4. **Prune `Assets/`.** 183 textures and meshes such as `MaleGuard` and `Security_Camera` are leftovers of the removed team heist game, catalogued in `UsedAssets.csv`, with no recorded provenance. The distributed roles load only `Cube.msh`, `sphere.msh` and a small number of shaders. Ship only those; drop the rest from the released tree. This also makes the artifact far smaller to clone.
5. Confirm the font licence (`Assets/Fonts/PressStart2P.fnt`) or replace it.

## 5. Decisions — settled 2026-08-16

1. **Delta repair: proper.** Implement a real client→server acknowledgement. This resolves V3 (dead deltas), V5 (`stateHistory` growth) and experiment 6 (client-perceived latency) in one piece of work instead of three, and it is the only option that makes experiment 5 measure something real.
2. **Ghost/halo is in scope, targeting January**, worked as fast as the fixes allow rather than held to a fixed date. See §5.1 — this is better motivated than it first appeared.
3. **Fix the 20-peer overflow** (`GameServer::mPeers`), and audit which past runs exceeded 20 total peers; those results are void.
4. **Aura Projection characterisation — resolved, see §5.2.** No factual error, but a different and more serious problem.

### 5.1 Ghost/halo closes a gap the dissertation itself opened

Thesis §3.3.1 (para 135) motivates the prediction system with *"collision of the objects that are in the different physics server borders."* The system has **no cross-border collision at all** — out-of-region objects are deactivated and skipped by broadphase.

So the halo layer is not scope creep bolted on for a reviewer. It delivers the capability the dissertation set out to deliver and resolved only by co-location. That framing also positions cleanly against both neighbours:

- **vs. Aura Projection** — AP explicitly rejected ghosting on cost grounds (*"a 'ghost' object takes up just as many resources in deriving a solution as a real object"*). Re-evaluating that trade-off on commodity hardware, with an owner-authoritative *kinematic* ghost that is deliberately cheaper than a full solve, is a legitimate and directly comparable result.
- **vs. Kale & Kry 2024** — their overlap-set blending is *"not physically correct"* by their own statement. An owner-authoritative halo with the Eibl & Rüde master/shadow rule (all transformations on the master, impulses reduced onto the master) is exactly correct for the bodies it covers, at the cost of not conserving momentum across the seam. Different trade-off, honestly stated.

### 5.2 Aura Projection: the thesis is vague, not wrong

The dissertation mentions AP exactly once, in para 28:

> *"(Brown et al. 2019) came up with a solution for separating responsibilities of physics engines by separating the areas of the virtual world and how to determine the transitions between distributed server areas by a system called Aura Projection."*

Accurate but contentless — and it is the **only** engagement with the nearest prior work, from the same institution, co-authored by a named referee. The paper's related-work section must do materially better: state AP's aura radius `R_a = R_o + V_t·T_T`, acknowledge that the `V_t·T_T` term is already a predictive lookahead, and locate this work's contribution in the *soundness envelope* rather than in the act of extrapolating (§2).

### 5.3 Defect: the thesis Algorithm does not match the implementation

Thesis para 150 documents constant-acceleration kinematics:

```
futurePosition := currentPosition + (currentVelocity × dt) + (0.5 × acceleration × dt²)
```

`PhysicsSystem::PredictFutureStateOfObject` (`PhysicsSystem.cpp:184-196`) implements semi-implicit (symplectic) Euler:

```cpp
linearVel += accel * dt;
Vector3 predictedPosition = transform->GetPosition() + linearVel * dt;   // = p + v·dt + a·dt²
```

**The acceleration term is 2× the documented one.** At the hardcoded 100 ms horizon under gravity this is ≈5 cm of additional predicted displacement per handoff — a systematic bias in exactly the quantity experiment 4 measures (handoff position error).

Resolution: keep the code (symplectic Euler is the better choice for stability) and **correct the algorithm in the write-up**. Also remove the hardcoded `0.1f` in `PredictFuturePositions` (the computed `dt` is currently discarded) so the horizon is a declared parameter rather than a magic number — it is a parameter the paper's soundness argument depends on.

---

## 6. Status and reassessment — 2026-08-26

Written after Phase C closed. §2's contribution claim is no longer a plan; the core of it is measured.
This section records where that leaves the paper, and what now limits it.

### 6.1 The §2 contribution claim is validated, and sharper than it was stated

§2 asked for a *"joint (lookahead horizon, halo width) condition ... that guarantees no missed
cross-boundary contact within a stated latency envelope"*, and called it genuinely unpublished and
defensible. As of 2026-08-25 it exists, is implemented as one shared definition
(`CSC8503CoreClasses/DistributedSystemCommonFiles/HaloBound.h`), and has been swept over its latency
term across 302 runs:

    w_min = v_max * (L * dt + T_L + T_J) + 2 * r_max

Three results, in descending order of how much they are worth to the paper:

1. **The latency envelope is measured rather than stated.** §2 wrote "within a stated latency
   envelope" as a hedge. It is now a number: the condition holds while **total** sample-to-apply lag
   stays under roughly 200 ms and fails above roughly 267 ms *at every width, including widths above
   the predicted floor*. Reached identically along two independent axes — lookahead 32 at zero
   latency and lookahead 40 at 300 ms both pin at 60 missed contacts of 100, flat across every width
   swept.
2. **The two terms are not interchangeable, and the obvious mitigation does not work.** Below the
   scheduling lookahead, injected latency does not move the required width *at all* — the lookahead
   is a delay budget that latency spends rather than adds to. Above it, the width rises but stays
   under the bound. Raising `--halo-lookahead` to absorb link latency was tested directly, at
   L=40/300 ms, and failed: it moves the lag from one term into the other and the ceiling is on
   their sum.
3. **A cost term is missing from the derivation.** Above 200 ms, monotonicity in width breaks —
   wider stops being safer. At L=24/200 ms, width 12 caught every contact and width 28, the bound's
   own prescription, did not, because publishing that many shadows over a delayed link costs late
   applications and bandwidth.

Result 2 is the most publishable sentence in the project: it is a negative result about a mechanism
everyone reaches for, it was arrived at by a prediction that was stated in advance and refuted, and
the refutation is what identifies the cause. Result 3 says the condition is necessary but the naive
reading of it — "when in doubt, widen" — is actively wrong near the envelope.

### 6.1a The bounds interact, and that is a result in itself (2026-08-27)

§6.1 states the halo soundness condition and its measured latency envelope. Phase D added a second
bound — the handoff lookahead needed for ownership atomicity — and the two are **not independent**.

Measured (`docs/superpowers/results/2026-08-27-D2-ownership-under-latency.md`, 39 runs, a
prediction stated in advance and confirmed on both ends):

- The lookahead required to close the ownership gap rises with link delay as `L_min ~ 4 + T_L/dt`.
  `L = 8` holds to 25 ms and collapses at 50; `L = 16` holds to 75 ms and fails at 100.
- At `T_L = 100 ms` the gap needs `L = 32`, while the halo bound forbids anything above 16 at
  `--halo-width 8`. **The run that closes the gap is the run that violates the other bound.**
- The band width that satisfies both is a **compound** neither states alone: 12 from halo soundness,
  16 from the lookahead, and the binding constraint is the interaction.
- And §6.1's result 3 already showed a wider band is not always safer under latency, so the
  compound requirement rises while the safe width does not.

**For the paper this is worth more than either bound separately.** A single soundness condition is
a contribution; two conditions on the same mechanism that provably conflict past a measured
threshold is a sharper and less expected one, and it is the kind of claim that is hard to obtain
without exactly the invariant-and-gate apparatus §6.3 describes. It also supplies a third instance
of the project's recurring shape: raising the halo lookahead cannot absorb link latency (§6.1
result 2), raising the handoff lookahead cannot absorb load (Phase D), and raising it to absorb
latency works only until it collides with another guarantee's bound (here).

### 6.2 What now limits the paper, in priority order

1. **It is one machine.** Everything runs as processes on loopback; latency is injected in-process,
   not measured. This was a modest weakness while the claims were about partitioning and throughput.
   It is now the *binding* weakness, because the headline result is about network delay and was
   measured with no network. Two or three physical machines on a LAN plus a WAN emulator would close
   it, and nothing else on this list buys as much.
2. **The ownership gap is the default behaviour, not an edge case.** At `--handoff-lookahead 0` —
   what essentially every experiment in `docs/EVALUATION.md` ran at — the sender releases on send and
   nobody owns the object for one network round trip; 1,397 of 1,800 ticks on a uniform run had an
   unowned object. It is documented honestly and §6 of EVALUATION states the guarantee as
   conditional, but a correctness paper resting on a default protocol with a known ownership hole
   invites exactly one reviewer question. **Phase D item 2 is worth more to the paper than any
   further sweep.**
3. **The halo bound rests on one synthetic workload.** `headon` is collision-dense at the border by
   construction, which is where linear dead reckoning over a long lag is worst — so the lag ceiling
   in §6.1 is plausibly a property of the workload rather than of the design. The optional companion
   in the Phase C spec (§4.7) is what would settle that, and it stopped being optional the moment the
   ceiling became a headline number.
4. **Scale and baselines.** 2 servers mostly, 4 occasionally; 100-400 objects; comparisons against
   *published* numbers with declared deviations rather than against a running baseline system.

### 6.3 An alternative framing worth considering

The current framing — a distributed physics server with partitioning, handoff and halo regions — sits
in a crowded space (§1.2, §1.3): AP, Colyseus, Kale & Kry, MMO zoning. The measured bound is the novel
part, and §2 is right to build on it.

But there is a second asset this project has that almost nobody else does, and it is currently
treated as process rather than contribution: **the validation apparatus**. Invariants I1-I8 checked
per tick and per run; a reproducibility gate distinguishing the 20 counters that reproduce from those
that do not; unit tests for the analysis tooling itself; and — most unusually — a documented record of
three separate occasions where a measurement artefact was mistaken for a system effect and then caught
(E5 round 1's sampling design, item 13's harness-lifetime change, and Phase C's own to-floor error).

A paper framed as *"how do you validate a distributed physics simulation, and what breaks when you
try"*, with the halo bound as the worked example, would suffer far less from §6.2's item 1 — the
contribution would be the method, for which one machine is a legitimate testbed. This is offered as an
option, not a decision. MMSys OSS & Datasets (11 Jan, §4) rewards exactly this kind of artifact, and
DS-RT and PADS both have reproducibility-minded audiences.

### 6.4 Where the work stands

| | Status |
|---|---|
| Phase A (instrumentation, harness) | Complete |
| Phase B (E4 attribution) | Complete |
| Phase C (latency + generalised bound) | **Complete**, 302 runs, both gates passed |
| Phase D (ownership: items 2, 7, 15) | **Complete** 2026-08-26. Items 7 and 15 closed; item 2 closes below the pacing budget and stays open above it |
| Backlog (`docs/EVALUATION.md` §7) | 15 items: **12 closed** (1, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15), **2 withdrawn** (3, 6), **1 conditional** (2 — closed below the pacing budget) |
| Jitter (`T_J`) sweep | Not started; implemented and asserted only |
| Oblique/mixed-speed workload (Phase C §4.7) | Not started |
| Multi-machine measurement | Not started — **not currently on any track** |

**Recommended ordering, revised 2026-08-26 after Phase D.** Item 2 is done, and what it found
changes the ranking underneath it. The ownership gap closes cleanly while servers hold their pacing
budget and does not close *at any lookahead* once they do not — 8, 16, 32 and 64 all leave ~1600 of
1800 ticks with an unowned object at 8,000 objects, where `phys_p95` is 10.2-10.4 ms against an
8.33 ms budget. That is the same tick-epoch divergence §6.2's item 1 is about, reached from a second
direction.

So **multi-machine measurement is now first by a wider margin than before**: it is no longer only
the fix for a confounded scaling segment, it is the experiment that would separate "the atomicity
guarantee is bounded by contention on one oversubscribed box" from "the atomicity guarantee is
bounded by load in general". Those are very different claims and this testbed cannot tell them
apart. Then the second halo workload; then jitter and further latency resolution, which remain the
lowest-value options for the same reason as before.

One experiment is cheap and was deferred rather than rejected: the Phase D lookahead sweep re-run
under `--link-latency-ms`. §5.1 of the backlog design argued D should follow C precisely because
injected latency makes the ownership gap proportional to link delay, and no latency was injected in
Phase D. It is the natural companion to the result and needs no new code.

The timeline in §4 assumed a November arXiv preprint. Nothing in Phase C changes that date, but the
priority list above does change what should be in it: a preprint carrying the measured lag envelope
and a fixed ownership protocol is a substantially stronger artifact than one carrying more sweep
points.
