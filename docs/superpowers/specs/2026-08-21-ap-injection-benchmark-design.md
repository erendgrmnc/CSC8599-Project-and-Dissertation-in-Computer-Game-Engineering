# AP-comparable injection benchmark (2026-08-21)

Date: 2026-08-21
Status: **design — approved, not yet implemented**
Follows: `2026-08-16-phd-paper-roadmap.md` (Track 2), `2026-08-19-evidence-completion-design.md`
Closes: the "no AP-comparable injection workload" gap in `docs/EVALUATION.md` §5

## 0. Why this exists

Every experiment E1–E8 measures this system against *itself* — one server against four, halo on
against halo off, one interest radius against another. None of them measures it against anything
anyone else built. That is the largest hole in the evaluation, larger than any open defect, because it
is the one a reviewer will name first.

Aura Projection (Brown, Ushaw & Morgan, I3D 2019) is the direct ancestor: distributed rigid-body
physics, spatial regions, boundary migration, from this institution. Its benchmark is fully specified
in the paper, so it can be **reproduced** even though no artifact was released. That is the cheapest
route from "no comparison" to "a bounded comparison against the ancestor".

**This is benchmark reproduction, not system execution.** No AP code exists to run. The result is
"same workload, same metric, published figures alongside" — never a head-to-head. §6 states the
deviations so that distinction survives contact with a reviewer.

## 1. AP's benchmark, as published

From §5 of the paper (Newcastle ePrints 255561), verbatim parameters:

| | |
|---|---|
| Duration and rate | 60 s at **160 objects/second** (~9,600 objects accumulated) |
| Object types | sphere (r = 0.3 m), cuboid (0.3 × 0.3 × 1.0 m), capsule (r = 0.3 m, h = 2 m), randomly selected |
| Initial velocity | uniform in (−10 < x < 10, −10 < y < 0, −10 < z < 10) m·s⁻¹ |
| Injection site A (50%) | volume 20 × 20 × 150 m, centred **12 m from a boundary**, 15 m above the ground plane |
| Injection site B (50%) | volume 20 × 20 × 20 m at **region centre**, 15 m above the ground plane |
| Speed tolerance `V_t` | 32 m·s⁻¹ |
| Latency tolerance `T_L` | 2 ms |
| Frame-time tolerance `T_F` | 15 ms |
| Physics step `T_P` | 16 ms |
| Metric | **max frame time of any server**, aggregated per **5 s** window, meaned over **50 repetitions** |
| Layouts | column (1–10 servers); corner (3; 4 = 2×2; 9 = 3×3) |
| Hardware | one AWS G2.2xlarge per server — Xeon E5-2670 2.60 GHz, 16 GB, GRID K520 |
| Stack | PhysX + RakNet; clients in Unreal Engine |

Headline result: one server reaches ~50 ms max frame time by t = 60 s; ten servers stay near 5 ms.

### 1.1 A finding worth more than the benchmark

AP's aura radius is

```
T_T = (3 * ceil((2*T_F + T_L) / T_P) - 1) * T_P
R_a = R_o + (V_t * T_T)
```

With their published tolerances, `T_T = (3*ceil(32/16) - 1)*16 ms = 80 ms`, so `R_a = R_o + 2.56 m`.

Compare this project's soundness condition (`ServerWorldManager::MinimumSafeHaloWidth`):

```
w_min = HALO_ASSUMED_MAX_SPEED * L * dt + 2 * HALO_ASSUMED_MAX_RADIUS
```

**AP's bound is parameterised by latency and frame-time tolerance. This one is not.** `w_min` accounts
only for the lookahead window `L * dt`. It is therefore a **zero-latency, zero-frame-jitter special
case** of the ancestor's condition, and AP's derivation shows the shape the general form must take:
the `ceil(... / T_P)` rounding to whole physics steps, the factor of three covering host displacement
plus remote displacement plus the detection delay, and the explicit `T_L` term.

This should be stated in the paper as a structural relationship, not discovered by a reviewer. It also
converts "no latency injection" (`docs/EVALUATION.md` §5) from a missing datapoint into a **correctness
gap in the headline claim** — E5 validates a bound whose latency term is absent because latency was
always zero.

## 2. Component 1 — the `Cuboid` archetype

`ObjectArchetype` currently holds `Cube = 0` and `Sphere = 1`. Add `Cuboid = 2`.

- `OBBVolume` with half-extents (0.15, 0.15, 0.5), giving AP's 0.3 × 0.3 × 1.0 m.
- `InitCubeInertia()`, not `InitSphereInertia(false)`.
- Wired into `CreateObjectFromArchetype` beside the existing two.

**`Cube = 0` is not modified.** It uses `AABBVolume` and sphere inertia, and every measurement in
E1–E8 was taken with it. Changing it would invalidate the entire evidence base; appending a new
archetype costs nothing on the wire, since the enum is append-only and the field is already an `int`.

**Capsules are omitted, and this is a physics limitation, not a shortcut.**
`CollisionDetection.cpp:201` carries a `//Two Capsules` comment above an empty gap: capsule-sphere,
capsule-AABB and capsule-OBB are implemented, capsule-capsule is not. AP makes capsules a third of
injected objects (~3,200 by t = 60 s), and they would pass through each other silently. Implementing
that primitive is work in the node-local solver, which this paper explicitly does not claim as a
contribution. Recorded as a declared deviation (§6) rather than absorbed as scope.

## 3. Component 2 — `--workload injection`

Every existing workload pre-seeds a fixed grid at world construction. This one **starts from an empty
world** and spawns over time through the existing `ICommandContext::SpawnObject` path.

### 3.1 Rate: 160/s total, not per server

AP states a flat 160 objects/second while sweeping 1–10 servers. If the rate scaled with server count,
total simulated work would scale with it too, and "more servers lowers frame time" would compare
different workloads — the headline would mean nothing. Holding the rate constant is the only reading
under which their result is a scalability claim.

**They do not say this outright.** It is recorded here as an interpretation with its reasoning, and it
must appear as such in the results document.

### 3.2 Determinism without a central spawner

The spawn schedule is a pure function of `(seed, objectIndex)`:

- **when** — object `i` is due at `t = i / 160.0` seconds
- **where** — site A (boundary-adjacent) when `i % 2 == 0`, site B (region centre) otherwise, giving
  the 50/50 split deterministically rather than by sampling
- **what** — archetype from `(i / 2) % 2`: sphere or cuboid
- **velocity** — drawn from a seeded generator keyed on `i` alone, so it does not depend on which
  server evaluates it

> **Site and type must not share a divisor.** Selecting both on `i % 2` would put every sphere at the
> boundary and every cuboid at the region centre — perfectly correlated, where AP draws type at random
> independently of site. Using `i % 2` for site and `(i / 2) % 2` for type walks the four combinations
> in turn, so each site receives an equal mix. Any future change to either rule has to preserve that
> independence.

Every server walks the same schedule and spawns **only** the objects whose site falls inside its own
region. No coordination, no duplicates, no central allocator — and it avoids one server owning the
entire population at t = 0 and triggering a handoff storm that would measure the wrong thing.

Runtime ids already carry the originating server in bits 29..22 (`NetworkIdSpace.h`), so two servers
spawning concurrently cannot collide.

### 3.3 Geometry

Site A is 20 m thick perpendicular to the boundary, 20 m tall, and 150 m long **parallel** to it,
centred 12 m from the boundary — matching Fig. 4, where boundary injection volumes are drawn as long
thin rectangles along the boundary. Site B is a 20 m cube at the region's centre. Both sit 15 m above
the ground plane; the world already has a floor (1000 × 2 × 1000) and gravity defaults to
(0, −9.8, 0).

## 4. Component 3 — the AP frame-time metric

### 4.1 What counts as a frame

A metric sample is currently recorded on **every loop iteration**, and in realtime mode the loop spins
at roughly 1 kHz while physics substeps at 120 Hz — so most rows did no physics at all. A loop
iteration is therefore not a frame in AP's sense, where `T_P = 16 ms` and the frame-time tolerance is
15 ms.

**Definition adopted:** a *frame* is a loop iteration in which physics actually advanced, and its
*frame time* is the wall-clock interval since the previous such iteration.

That interval includes everything real that happened in between — networking, snapshot broadcast, halo
publication, spin — which is what AP's frame time measures. Defining it as `physics_ms` alone would
exclude the costs a distributed system actually pays and would flatter the result.

This needs one new `TickSample` column, `substeps` (the count executed that iteration), so the filter
is unambiguous. Keying off `physics_ms > 0` is not sufficient: it rounds to zero.

### 4.2 Aggregation

Per AP: within each 5 s window, take the **maximum** frame time observed across **all** servers; then
take the **mean** of those per-window maxima across repeats. Emitted by `analyse.py` as a table of
`(elapsed_seconds_bucket, servers, mean_max_frame_ms)` — directly plottable against AP's Fig. 5.

Max-across-servers, not per-server: the metric is deliberately about the worst participant, since a
distributed frame is only complete when the slowest server finishes.

## 5. Run configuration and what each phase delivers

Realtime (`--run-seconds 60`), never paced — the claim is about frame time under accumulating load,
and paced mode would report whatever the pacing allowed. `--drain-seconds 0`, since the drain sits
outside the measured window.

**Phase 1, single machine.** Validates the workload and the metric, and characterises the machine.
It cannot produce the comparison graph: AP measured one server per AWS instance, whereas adding
servers on one 6-core box adds contention, so the curve would argue *against* the design. Phase 1's
deliverables are a working benchmark, a validated metric, and a per-machine baseline.

It also doubles as a stress test. 160/s × 60 s = 9,600 objects against a measured correctness budget
of ~3,384 objects/server (E7), so a 2-server run crosses the budget at roughly t = 42 s. Expect
custody to engage; `hoResent`, `hoReclaimed`, `hoCustody` and `conservation_delta` are part of the
result, not noise.

**Phase 2, two machines.** Server counts 1 and 2, one per machine, wired ethernet (WiFi jitter runs
tens of milliseconds and would swamp AP's 2 ms latency tolerance). Yields a two-point curve, not AP's
1–10 sweep.

The two machines are **not identical**, which AP's were. This must be controlled by running the
single-server baseline on each machine separately and reporting their relative capability, because the
metric is defined by the *worst* server. It is also an opportunity: the load balancer equalises object
counts, not capability, so on heterogeneous hardware equal object counts are not equal load — a
systematic overload of the slower machine that neither AP nor Kale & Kry tested. That connects
directly to the standing limitation that the load profile buckets objects rather than measured cost.

Repeats: 5–10 rather than AP's 50, declared.

## 6. Declared deviations

"AP-comparable" must name what was matched, or it means nothing. Every item below belongs in the
results document:

| | AP | here |
|---|---|---|
| Object types | sphere, cuboid, capsule | sphere, cuboid — **capsule-capsule collision unimplemented** |
| Physics step | 16 ms (62.5 Hz) | 8.33 ms (120 Hz) |
| Speed tolerance | 32 m·s⁻¹ | 60 (`HALO_ASSUMED_MAX_SPEED`, hardcoded) |
| Boundary mechanism | aura sphere per object, migration on aura collision | halo band per region, handoff on region exit |
| Networking | RakNet | ENet |
| Solver | PhysX | CSC8503 engine (uniform-grid broadphase, no sleeping, no islands) |
| Hardware | 10 identical AWS G2.2xlarge | 2 heterogeneous commodity machines |
| Server counts | 1–10 column, 3/4/9 corner | 1–2 column only |
| Repeats | 50 | 5–10 |
| Latency | ~2 ms measured, and **in the aura formula** | ~0 ms LAN, **absent from the halo formula** (§1.1) |

The last row is the one that matters most, and it is a finding rather than an excuse.

## 7. Out of scope

- **Implementing capsule-capsule collision.** Node-local solver work; not claimed by this paper.
- **Corner layouts.** Needs ≥3 machines; the column layout at N=2 is what the hardware supports.
- **Matching AP's 16 ms physics step.** Changing the substep rate would invalidate E1–E8. The
  difference is declared instead.
- **Latency injection.** Identified in §1.1 as the more important gap, but it is its own piece of work
  and belongs in its own spec — it changes what E5 claims, not just what this benchmark measures.
