# AP-comparable injection benchmark — Phase 1 results (2026-08-21)

Spec: `docs/superpowers/specs/2026-08-21-ap-injection-benchmark-design.md`
Plan: `docs/superpowers/plans/2026-08-21-ap-injection-benchmark.md`
Dataset: `runs/exp-ap-injection-paced` (gitignored), commit `e413ab0`

**Status: 1 server measured and sound. 2 servers NOT reportable** — a peer link is lost
mid-run and never re-established (§5). The scalability half of the comparison is blocked on
that defect, not on the workload.

---

## 1. What this is, and what it is not

**This is benchmark reproduction, not system execution.** No Aura Projection code exists to
run; nothing here is a head-to-head. The result is "same workload, same metric, published
figures alongside", and §4 states every deviation so that distinction survives review.

Every experiment E1–E8 measures this system against *itself*. This is the first measurement
against anything anyone else built.

## 2. AP's published parameters against the measured configuration

| | AP (I3D 2019, §5) | measured here |
|---|---|---|
| Duration and rate | 60 s at 160 objects/s | 60 s **simulated** at 160/s |
| Objects accumulated | ~9,600 | **9,601 spawned, 9,599 owned at exit** |
| Object types | sphere, cuboid, capsule | sphere, cuboid (§4) |
| Initial velocity | uniform (−10<x<10, −10<y<0, −10<z<10) m/s | identical |
| Site A (50%) | 20 × 20 × 150 m, 12 m from a boundary, 15 m up | identical, on an interior boundary |
| Site B (50%) | 20 × 20 × 20 m at region centre, 15 m up | identical |
| Metric | max frame time of any server, per 5 s window | identical, windows in simulated time (§3.2) |
| Repeats | 50 | 5 |
| Servers | 1–10 column | **1 only** (§5) |

Seed 42, world −150,150 / −150,150, `--fixed-step`, `--run-ticks 7200`, halo width 8,
halo lookahead 4, reliable halo, handoff lookahead 300, drain 10 s.

## 3. Result — 1 server

### 3.1 The workload was delivered exactly

All five repeats are identical: 7,200 ticks, 60.0 s simulated, 9,599 objects owned at exit
against 9,601 spawned. `hoSent = hoRecv = hoFail = 0` (one server has no peers), 38.2 M
contacts resolved. Conservation is −2 objects across the run.

That the population lands on the same value in all five repeats is the evidence that the
schedule is genuinely a pure function of `(seed, index)`.

### 3.2 Max frame time per 5 s window

Windows are **simulated** seconds; frame time is wall-clock milliseconds. AP's x-axis indexes
the injected population — at *t* seconds their world holds 160·*t* objects — so a window of
ours only compares against theirs if it sits at the same point in the schedule. This matters
here because the run does not keep up in real time (§3.3).

| window (s) | objects present | max frame time (ms) | AP, 1 server |
|---|---|---|---|
| 0–5 | 800 | 15.5 | |
| 5–10 | 1,600 | 22.5 | |
| 10–15 | 2,400 | 22.5 | |
| 15–20 | 3,200 | 33.7 | |
| 20–25 | 4,000 | 56.7 | |
| 25–30 | 4,800 | 78.3 | |
| 30–35 | 5,600 | 90.9 | |
| 35–40 | 6,400 | 95.0 | |
| 40–45 | 7,200 | 126.6 | |
| 45–50 | 8,000 | 138.2 | |
| 50–55 | 8,800 | 133.8 | |
| 55–60 | 9,600 | **150.0** | **~50** |

Per-tick physics cost, median across repeats: p50 22.0 ms, p95 52.4 ms, p99 61.4 ms.

**Reading.** The shape matches AP's — frame time rising roughly linearly with the injected
population — and the level is about **3× worse** at the 60 s mark: 150 ms against their ~50 ms.
That gap is not surprising and is not a like-for-like defeat: AP ran PhysX on a dedicated
AWS G2.2xlarge, this is the CSC8503 engine (uniform-grid broadphase, no sleeping, no islands,
no sub-stepping tuned for contact-heavy piles) on a shared commodity desktop that is also
hosting the manager, midware and client. The comparison worth making is the *shape*, and the
per-object cost, not the absolute millisecond count.

The floor early in the run is the physics step (8.33 ms at 120 Hz), not a measurement
artifact — see the note `analyse.py` prints under the table.

### 3.3 The machine does not sustain AP's rate in real time

60 s of simulated injection took **189–288 s of wall clock** (median 202 s). The run is paced,
so simulated time is exact and the workload is delivered in full, but the server runs at
roughly 30% of real time by the end.

Stated plainly: **this machine cannot sustain 160 objects/second at one server.** AP's server
could not either — that is why their 1-server frame time reaches 50 ms — but theirs kept
injecting in real time while ours pins the schedule to simulated time. The two are equivalent
as a *load* curve (both report frame time at a known population) and not equivalent as a
*responsiveness* curve.

## 4. Declared deviations

"AP-comparable" must name what was matched or it means nothing.

| | AP | here |
|---|---|---|
| Object types | sphere, cuboid, capsule | sphere, cuboid — **capsule-capsule collision unimplemented** |
| Physics step | 16 ms (62.5 Hz) | 8.33 ms (120 Hz) |
| Speed tolerance | 32 m/s | 60 (`HALO_ASSUMED_MAX_SPEED`, hardcoded) |
| Boundary mechanism | aura sphere per object, migration on aura collision | halo band per region, handoff on region exit |
| Networking | RakNet | ENet |
| Solver | PhysX | CSC8503 engine (uniform-grid broadphase, no sleeping, no islands) |
| Hardware | 10 identical AWS G2.2xlarge | 1 commodity desktop, shared with every other role |
| Server counts | 1–10 column, 3/4/9 corner | **1** (2 blocked, §5) |
| Repeats | 50 | 5 |
| Latency | ~2 ms measured, and **in the aura formula** | ~0 ms LAN, **absent from the halo formula** (§6) |
| Injection clock | real time | simulated time (§3.3) |

Capsules are omitted because `CollisionDetection.cpp` implements capsule-sphere, capsule-AABB
and capsule-OBB but **not** capsule-capsule. AP makes capsules a third of injected objects
(~3,200 by t = 60 s) and they would pass through each other silently. That primitive is work
in the node-local solver, which this paper does not claim as a contribution.

**Phase 1 is single-machine and therefore cannot reproduce AP's scalability claim at all.**
Adding servers on one box adds contention rather than capacity. Phase 1 validates the
workload, the metric, and the per-machine baseline; the scalability comparison needs Phase 2
hardware.

## 5. Why 2 servers is not reported

In every 2-server repeat, one server loses its peer link to the other roughly 13 s into the
run and never recovers it. The consequences compound:

1. Every subsequent handoff to that peer fails. The object is correctly *retained* rather than
   lost, so conservation holds — but the partition stops functioning as a partition.
2. The peer's snapshot-interest declaration ("send me nothing") stops arriving, so the
   affected server floods its peer with snapshots that are decoded and discarded. Measured as
   suppression rate: **50.0% on the healthy server, 15–18% on the affected one**, in all five
   repeats.
3. The affected server takes **~800 s** of wall clock against ~180 s for its peer. In one
   repeat of five it never finished at all and produced no metrics.

The correlation is exact across all nine server-runs that completed: low suppression ⟺ ~800 s,
50% suppression ⟺ ~180 s. It is **not** a fixed server id — it was server 0 in four repeats and
server 1 in the fifth — so it is a race, not a wiring error.

`RetryPendingPeers` only retries peers that failed their *initial* connect; `mPendingPeers` is
never repopulated when an established link drops. **A dropped peer link is permanent.**

Reproduced on three further independent runs after the two logging fixes below. The affected
server was server 0 in five of the eight observations and server 1 in three, and in three of
them it never finished inside the harness's 900 s ceiling and produced no metrics at all.
Cutting the log volume by 88x (715,702 lines to 8,077) changed neither the drop nor the
outcome, which is what establishes the logging as an amplifier rather than the cause.

This is a genuine system defect, not a harness artifact, and fixing it needs design work:
re-establishing a link mid-run has to interact correctly with handoff custody, the ownership
invariants, and reproducibility. It is recorded as a backlog item rather than patched here.

> Two contributing factors were fixed while diagnosing this, neither of which is the cause:
> the failure logged once per object per tick (233,654 lines down the midware pipe in a single
> run, enough I/O to perturb the measurement it was reporting on — now rate-limited, `45a4838`),
> and the harness killed the client long before the servers finished, which caused *unrelated*
> multi-second stalls from reliable broadcasts to a dead peer (`e413ab0`).

## 6. The finding that outlives the benchmark

AP's aura radius is parameterised by latency and frame-time tolerance:

```
T_T = (3 * ceil((2*T_F + T_L) / T_P) - 1) * T_P
R_a = R_o + V_t * T_T
```

With their published tolerances `T_T = 80 ms`, so `R_a = R_o + 2.56 m`.

This project's soundness condition (`ServerWorldManager::MinimumSafeHaloWidth`) is

```
w_min = HALO_ASSUMED_MAX_SPEED * L * dt + 2 * HALO_ASSUMED_MAX_RADIUS
```

which accounts only for the lookahead window `L·dt`. **It is a zero-latency, zero-frame-jitter
special case of the ancestor's condition.** AP's derivation shows the shape the general form
must take: the `ceil(…/T_P)` rounding to whole physics steps, the factor of three covering host
displacement plus remote displacement plus detection delay, and an explicit `T_L` term.

This should appear in the paper as a structural relationship rather than be found by a
reviewer. It also converts "no latency injection" (`docs/EVALUATION.md` §5) from a missing
datapoint into a **correctness gap in the headline claim**: E5 validates a bound whose latency
term is absent because latency was always zero.

## 7. Defects found by running this benchmark

Listed because they are the substantive output of Phase 1 alongside the numbers. Every one was
found by *running* the workload, not by reading code.

| | Defect | Status |
|---|---|---|
| 1 | `InjectionPosition` mapped every draw onto the evaluating server's own region, making the ownership test vacuous — every server spawned every draw, multiplying the injection rate by the server count (3,178 objects where 1,600 was due) | fixed, `b0467b4` |
| 2 | `DrainPendingSpawns` ran only from `DispatchCommand`, so a run with no client commands never counted or broadcast a single runtime spawn | fixed, `87205ea` |
| 3 | `analyse.py` used the final population as the I1 per-tick baseline, reporting 1,199 of 1,200 ticks as ownership gaps on a world that fills from empty | fixed, `2db5044` |
| 4 | Client lifetime was sized in simulated seconds, so it died mid-run and left servers broadcasting reliable packets at a dead peer — 34 s of blocking against 10 s of work | fixed, `e413ab0` |
| 5 | The "no peer link" error logged once per object per tick | fixed, `45a4838` |
| 6 | Both handoff traces printed BEFORE the send, so a down link reprinted them once per pending object per tick — 470,753 lines against 400 real handoffs, one of them from inside a packet constructor | fixed, `dd63915` |
| 7 | **A dropped peer link is never re-established** | **open — blocks §5** |

Items 5 and 6 share a second theme worth stating: **an error path that retries every tick
must not log every tick.** Both wrote more than half a million lines down the midware pipe on
a single run, which is enough I/O to perturb the measurement they were reporting on.

Items 1, 3 and 4 are the same underlying mistake in three places: **assuming wall-clock time
and simulated time are interchangeable.** They are not, and under load they diverge by a factor
of three. That is worth treating as a systemic hazard in this codebase rather than as three
unrelated bugs.
