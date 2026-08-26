# Phase D — ownership (items 2, 7, 15)

**Plan:** `docs/superpowers/plans/2026-08-26-phase-d-ownership.md`
**Spec:** `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` §5 (incomplete for
item 15; §5.8 is written at the end of the phase from what shipped)

Written incrementally as the phase runs. Sections are added as their task completes.

---

## Step 0 — the baselines

**Commit:** `5411f57`, clean tree. Release build of all four roles, exit 0.

Two configurations, because the first one chosen was vacuous and that is worth recording
rather than quietly replacing.

### The workload error, caught before execution

The plan originally specified `--workload seam` for every 4-server run, reasoning that seam
places objects exactly on the interior Z seam of the 2×2 grid — the coordinate item 7's defect
targets. It does. It then leaves them there: `seam` falls through the velocity assignment at
`ServerWorldManager.cpp:1159`, where everything except `shuttle` and `uniform` gets no velocity
at all.

Measured, 4 servers, 2 repeats (`runs/exp-d0-probe4`):

| counter | every server |
|---|---|
| `hoSent` | 0 |
| `hoRecv` | 0 |
| `hoClamp` | 0 |

`analyse.py` exits 0 and reports "all invariants hold". That verdict is true and empty: with no
handoffs there was nothing for the handoff path to get wrong. Had this stood as the gate
baseline, Gate A at 4 servers would have compared two runs with the entire handoff path
switched off, and passed — the same shape as backlog item 14, where invariant I4 passed
vacuously in every run of a phase because no client ever printed `@@FINAL`.

Recorded as plan ruling D6.

**What the seam run does establish**, and it is not nothing: `owned = 100` on each of the four
servers, 400 total, `mismatch = 0`. With a whole row and column of objects sitting exactly on
`x = 0` and `z = 0`, the half-open rule assigns every seam point to exactly one server. That is
`OwningServerFor`'s reason for existing and it had never been tested at a 2-D seam. It is kept
as a placement check (plan Task 0 Step 4b), not as a gate baseline.

### The baseline that exercises the path

`--workload uniform`, 4 servers, 400 objects, 1800 paced ticks, `--halo-width 8`,
`--halo-reliable`, `--handoff-lookahead 0`, 30 s drain, 2 repeats (`runs/exp-d0-probe4u`).

`uniform` is the only bundled workload that both spreads across the whole world and moves. Its
`lateralZ` component is ±10 units/s (`SHUTTLE_Z_SPREAD = 20.0f`), which over 15 simulated
seconds carries objects across the grid's `z = 0` seam as well as its `x = 0` one.

| | r1 | r2 |
|---|---|---|
| `hoSent` ↔ `hoRecv` | 92 ↔ 92 | 94 ↔ 94 |
| `objPool` total | 400 | 400 |
| `hoCustody` | 0 | 0 |
| `hoDup` | 0 | 0 |
| `hoClamp` (servers 0 / 2) | 5 / 3 | 5 / 3 |
| **`ownership_gap_ticks`** | **105** | **102** |
| `ownership_double_ticks` | 0 | 0 |

`analyse.py` exits 1 on one invariant: `ownership_gap_ticks`.

**That failure is the baseline, not a broken baseline.** It is the documented release-on-send
behaviour at `--handoff-lookahead 0` — `ScheduleOutgoingObject` hands off immediately and nobody
owns the object for one network round trip — which is precisely what item 2 exists to close.
Phase A accepted the same thing at 2 servers under its ruling P6 (`ownership_gap_ticks = 84`).
This is the first measurement of it at 4.

`ownership_double_ticks` reads 0 in both repeats: at lookahead 0 the 2-D seam did **not**
produce two simultaneous owners. Worth stating, because a 2-D seam was the plausible mechanism
for one and it did not appear.

---

## Task 1 — the arrival clamp's Z bound

**Commit:** `dc3127d`. Item 7, first half: the clamp is made correct, and is still discarded.

`CalculateIncomingObjectOffsetPosition` kept its own copy of the region bounds and clamped Z
with an inclusive upper bound. Its own comment claimed the bounds "mirror `IsObjectInBorder`
exactly — half-open on X, closed on Z", which was true until `IsObjectInBorder` began
delegating to `OwningServerFor`, half-open on **both** axes. On an interior Z seam the clamp
therefore returned a coordinate a different server owns.

The geometry moved to `NCL::Interaction::ClampIntoRegion`, in `RegionOwnership.h` beside the
rule it has to agree with, and both its inputs now come from `GetRegionBounds()` — the same
partition `GetObjectServer` feeds to `OwningServerFor` — rather than from `mServerBorderData`.
Reading the region from one source and the rule from another is how the two drifted apart.

### The tests discriminate

Seven cases appended to `tools/InteractionTests/RegionOwnershipTests.cpp`, reusing its existing
`TwoServerWorld()` / `FourServerWorld()` fixtures rather than duplicating them — a second copy
of a partition definition drifting from the first is the defect being fixed.

| stage | result |
|---|---|
| before implementation | compile failure, `'ClampIntoRegion': identifier not found`, all 7 |
| after implementation | 150 passed, 0 failed |
| **Z bound reverted to the pre-fix inclusive form** | **2 failed**, and only those 2 |

The two that fail are `ClampedPointOnAnInteriorZSeamIsOwnedByThisServer` and
`ClampedPointPastAnInteriorZSeamIsOwnedByThisServer`. `ClampedPointOnTheWorldOuterZEdgeStaysOnIt`
and `TwoServerSplitIsUnaffectedByTheZFix` keep passing against the defect, which is what
establishes that the fix does not disturb the closed-outer-edge exception every 2-server
conservation figure on record depends on.

### It is not a no-op, and the gate cannot see that

The commit message for `dc3127d` says the change "changes no measured behaviour". That is right
about *simulated* behaviour — the clamp's result is still discarded — and wrong about the
counters. `hoClamp` counts arrivals the clamp *would* move, so changing the clamp changes the
count. `hoClamp` is in neither `gate-compare.py`'s `STABLE` nor its `CONSERVED` list, so Gate A
is structurally incapable of catching it.

Measured directly instead. Same configuration as the baseline, 2 repeats
(`runs/exp-d1-clampcheck`), against the post-Task-1 binary:

| server | grid position | interior Z seam? | `hoClamp` before | `hoClamp` after |
|---|---|---|---|---|
| 0 | row 0, col 0 | yes (`maxZ = 0`) | 5 | 5 |
| **1** | **row 0, col 1** | **yes (`maxZ = 0`)** | **0** | **2** |
| 2 | row 1, col 0 | no (`maxZ = worldMaxZ`) | 3 | 3 |
| 3 | row 1, col 1 | no (`maxZ = worldMaxZ`) | 0 | 0 |

Reproducible across both repeats. The change lands on server 1 and nowhere else, and server 1
is in row 0 — the row whose `maxZ` is interior. Servers 2 and 3 sit against the world's outer Z
edge, where the closed-edge exception applies and the fix is a no-op by construction. The
predicted set and the measured set agree exactly.

**What the +2 means.** Pre-fix, an arrival landing at `z = 0` on server 1 clamped to `z = 0`,
which is unchanged, so nothing was counted — and `z = 0` belongs to row 1, not to server 1. Two
arrivals per run were therefore being installed on a coordinate the receiving server does not
own, and the old clamp would have left them there. That is the disowned-object case the
ownership unification exists to prevent, measured rather than argued, and it is reachable on a
healthy-path configuration at 4 servers.

Conservation and parity are unaffected: 400 objects both repeats, `hoSent` = `hoRecv` = 93.
`ownership_gap_ticks` reads 105 and 100 against the baseline's 105 and 102 — the same defect,
untouched, as expected at `--handoff-lookahead 0`.

---

## Step 0 (continued) — the 2-server baseline, and a bimodal ownership gap

`--workload uniform`, 2 servers, 400 objects, 1800 paced ticks, `--halo-width 8`,
`--halo-reliable`, `--handoff-lookahead 0`, 30 s drain, **4 repeats** (`runs/exp-d0-base2`).

**Deviation, recorded rather than hidden:** this baseline was taken at `dc3127d`, after Task 1,
not at the step-0 commit `5411f57`. Task 1 is inert at 2 servers by construction — the split is
1-D, so there is no interior Z seam and the closed-outer-edge exception covers the whole Z axis
— and the data confirms it: `hoClamp` reads **8 on server 0 and 0 on server 1 in all four
repeats**, matching `docs/EVALUATION.md` §7 item 7's record of 7–8 across the four Phase A
pre-change repeats and 8, 8, 8 across its three gate repeats. The clamp fix moved nothing here,
which is exactly what `TwoServerSplitIsUnaffectedByTheZFix` asserts.

Conservation is exact on every repeat: 198+202, 200+200, 197+203, 198+202 — 400 each time.

### The finding: `ownership_gap_ticks` is bimodal, not ~85

| repeat | `ownership_gap_ticks` (of 1800) | `ownership_double_ticks` |
|---|---|---|
| r1 | **1743** | **1** |
| r2 | 87 | 0 |
| r3 | 86 | 0 |
| r4 | 85 | 0 |

Three repeats sit at 85–87. One sits at 1743 — an object owned by nobody for **97% of the
run** — and it is the only repeat that also produced a double-owner tick.

This is not a new mechanism. `CLAUDE.md` already records "1,397 of 1,800 ticks on a uniform run
had an object owned by nobody, up to 33 at once", and Phase A's ruling P6 accepted
`ownership_gap_ticks = 84` as the documented baseline. What was not recorded is that **these are
two modes of the same configuration**, not two different configurations: the same seed, the same
tick count, the same binary, four repeats, and the gap lands either at ~85 or at ~1700.

Phase A saw the low mode and characterised the defect from it. This baseline caught both.

### Consequence for the plan: Task 5's repeat count is wrong

Task 5 sweeps `--handoff-lookahead` over L ∈ {0, 2, 4, 8, 16} at **3 repeats** and picks the
smallest L with `ownership_gap_ticks = 0`. Against a distribution that produces its bad mode
roughly one repeat in four, three repeats have a fair chance of returning three clean runs at an
L that has not in fact closed the gap — and the sweep would then report that L as the answer.

That is structurally the same trap as backlog item 15, which appeared in 1 of 3 repeats and
needed 6 to be seen at all. Task 5 is raised to **6 repeats**, and the sweep must report the
per-repeat spread, not a median: a median of 6 repeats would show 0 while two of them failed.
