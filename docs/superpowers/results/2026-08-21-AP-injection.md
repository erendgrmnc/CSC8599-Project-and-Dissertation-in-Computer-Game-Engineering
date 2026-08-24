# AP-comparable injection benchmark — Phase 1 results (2026-08-21)

Spec: `docs/superpowers/specs/2026-08-21-ap-injection-benchmark-design.md`
Plan: `docs/superpowers/plans/2026-08-21-ap-injection-benchmark.md`
Dataset: `runs/exp-ap-injection-paced` (gitignored), commit `e413ab0`

**Status: 1 and 2 servers both measured and sound.** The defects that blocked the 2-server
configuration are fixed (§5); it now runs to completion with balanced load, zero link drops and
clean invariants. §5.1 adds a 1/2/4-server scaling breakdown, which is the more useful result:
**the physics scales very nearly linearly, and the coordination overhead does not.**

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
| Servers | 1–10 column | 1, 2, and a 4-server scaling point (§5.1) |

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
| Server counts | 1–10 column, 3/4/9 corner | 1 and 2 measured, 4 profiled (§5.1) |
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

## 5. The 2-server configuration, and what blocked it

It was blocked by three defects, all now fixed. They are recorded because the way they hid
behind one another is the substantive part.

The visible symptom was that one server of the pair took ~800 s of wall clock against ~180 s
for its peer, suppressed 12% of snapshots to that peer against the peer's 50%, held zero halo
shadows, and in three runs of eight never finished at all. It was not a fixed server id — it
struck whichever server lost a race — and the peer link dropped repeatedly with ENet reason 0,
a timeout.

All of it came from **one** cause. `FlushScheduledHaloUpdates` re-sorted its entire pending
queue every tick, due or not, and erased applied entries one at a time from the middle:
O(n log n) plus O(k·n) against a backlog that grows whenever a server's tick counter falls
behind its peer's, because the peer's updates carry the peer's tick and land in the future.
That is a runaway — fall behind, accumulate backlog, sort costs more, fall further behind. It
reached 27–42 ms per tick in that one function against an 8.33 ms budget.

Everything else was downstream: the loop stopped servicing ENet for hundreds of milliseconds,
so the peer link timed out; the tick counters diverged past the staleness window, so halo
shadows died; and the peer-interest declaration was lost in the reconnect churn, so snapshots
were no longer suppressed.

Two further defects were real but not the cause, and were fixed on the way:

- **A dropped peer link was never re-established.** `GameClient::UpdateClient` had no
  `ENET_EVENT_TYPE_DISCONNECT` case at all, so a dropped peer left the client reporting a
  healthy link while every send was refused, and custody — which reclaims only when the link
  is genuinely gone — could never fire.
- **Declared snapshot interest was never cleared when a peer left.** ENet reuses peer numbers,
  so the next occupant of a slot inherited the previous one's declaration: a client landing
  where a server had been is starved of snapshots, a server landing where a client had been is
  sent the whole world every tick.

Measured on 2 servers × 7,200 paced ticks, before → after:

| | before | after |
|---|---|---|
| slow world-ticks (>25 ms) | 60 | **0** |
| peer link drops | 8–9 | **0** |
| snapshot suppression | 50.0% / 12.4% | **50.0% / 50.0%** |
| wall clock | 610 s / 185 s | **141 s / 140 s** |
| halo shadows held at exit | 0 / 65 | 779 / 644 |
| per-server contact ratio | — | 0.983 |
| invariants | conservation −175 | conservation −2 of 9,601 |

### 5.1 What scales, and what does not

Injection at a fixed **total** 160/s, 3,600 paced ticks (30 s simulated, 4,800 objects), swept
over server count on one 6-core machine:

| servers | tick period | **physics** | coordination overhead | objects/server |
|---|---|---|---|---|
| 1 | 14.01 ms | **9.87 ms** | 3.94 ms | 4,799 |
| 2 | 9.23 ms | **4.39 ms** | 4.72 ms | 2,400 |
| 4 | 9.05 ms | **2.34 ms** | 6.57 ms | 1,200 |

**The physics scales very nearly linearly** — 2.25× then 1.88× per doubling — which is the
claim the partition exists to support. At 4 servers the simulation costs 2.34 ms against an
8.33 ms budget.

**The coordination overhead does not scale**: 3.94 → 4.72 → 6.57 ms. Going from 2 servers to 4
saves 2.05 ms of physics and spends 1.85 ms more on coordination, so the total barely moves
(9.23 → 9.05 ms). Wall clock for the same 30 s of simulation went 50.4 s → 34.8 s → 34.6 s.

That flat segment is **not** a limit of the simulation. On this machine four servers plus the
manager, midware and client are seven processes on six cores, and the overhead term carries
that contention as well as the genuine per-peer protocol cost. Two supporting observations:
`haloLate` *improves* with server count (23.0% → 14.2%), which is lower per-server load
helping; while inter-server tick drift *worsens* (20 → 105 ticks), which is contention making
progress uneven. Separating those two contributions needs the multi-machine setup — but the
overhead is attributable to named work (halo publication, snapshots, handoffs), so it is an
optimisation target rather than a wall.

### 5.2 The limit that remains

`haloLate` stays high on whichever server runs ahead — 84% at 2 servers on the full 60 s run.
It measures **tick-epoch divergence**, not delivery jitter: `applyAt` is expressed in the
sender's tick numbers, and two servers share no epoch once either stops holding its pacing
budget. On the full AP load the counters start together and diverge monotonically to 89 ticks.

Both sides degrade, in mirror image: the faster server applies stale samples, the slower one
withheld fresh samples for the whole offset. The second of those is now bounded — a sample is
never withheld for longer than it could usefully be extrapolated — which clamped 1.87 M of
2.85 M arrivals and moved the per-server contact ratio from 0.979 to 0.983.

The divergence itself is not fixable at this layer, and invariant **I8 is unattainable while it
persists**: the lookahead exists precisely so both servers apply an update on the same
simulated tick. The cure is both servers holding their pacing budget, which this machine does
not at 9,600 objects (56–58 ticks/s against 120/s). Any 2-server figure from the full AP load
must carry that caveat.

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

> **DONE 2026-08-25 (Phase C).** The generalisation predicted here landed as
>
>     w_min = v_max * (L * dt + T_L + T_J) + 2 * r_max
>
> in `CSC8503CoreClasses/DistributedSystemCommonFiles/HaloBound.h`, with `--link-latency-ms` and
> `--link-jitter-ms` injecting delay on the server-to-server path. The relationship to AP's form is
> written out in that header: same shape - travel distance at maximum speed over a total delay, plus
> body radii - expressed in substeps rather than rounded up to whole periods `T_P`, because the halo
> schedules in the sender's tick numbers and so has no rounding to do, and without the `2*T_F` frame
> term because a halo update is published once per tick from the same loop that steps the world, so
> frame time is already inside `L * dt`. Injected jitter is what stands in for frame-to-frame
> variability.
>
> The **code** gap is closed; the **evidence** gap is not. Every measurement on record is still the
> `T_L = 0` case, and stays so until the E5 latency sweep runs.

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
| 7 | A dropped peer link was never re-established: `GameClient::UpdateClient` had no `ENET_EVENT_TYPE_DISCONNECT` case | fixed, `d47d12b` |
| 8 | `FlushScheduledHaloUpdates` re-sorted its whole pending queue every tick — a runaway that cost 27–42 ms/tick and caused every 2-server symptom (§5) | fixed, `35f5e20` |
| 9 | Declared snapshot interest was never cleared when a peer left, so a reused peer slot inherited it | fixed, `35f5e20` |
| 10 | Halo updates could be scheduled arbitrarily far ahead, so a slower server withheld fresh samples for 89 ticks | fixed, `37a2f3c` |
| 11 | **Tick-epoch divergence between servers under load** (§5.2) | **open — bounded, not cured** |

Items 5 and 6 share a second theme worth stating: **an error path that retries every tick
must not log every tick.** Both wrote more than half a million lines down the midware pipe on
a single run, which is enough I/O to perturb the measurement they were reporting on.

Items 1, 3 and 4 are the same underlying mistake in three places: **assuming wall-clock time
and simulated time are interchangeable.** They are not, and under load they diverge by a factor
of three. That is worth treating as a systemic hazard in this codebase rather than as three
unrelated bugs.
