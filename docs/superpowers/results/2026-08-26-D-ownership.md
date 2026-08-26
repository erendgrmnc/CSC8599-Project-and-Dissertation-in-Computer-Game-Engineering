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

---

## Tasks 2 and 3, and Gate A

**Commits:** `6cce34d` (Task 2, clamp applied), `1d19c2c` (Task 3, item 15's resend guard).

### The gate was split three ways, not two

Plan ruling D1 says Gate A runs after Tasks 1 and 3 and must reproduce the baseline
exactly, with Task 2 measured against it. The task numbering contradicts that — Gate A is
Task 4, so Task 2 lands before it — and execution followed the numbering. Rather than
conflate the two changes in one gate, each was isolated against the run that preceded it:

| comparison | isolates | verdict |
|---|---|---|
| `exp-d0-base2` → `exp-d2-wired2` | Task 2 (clamp applied) | **PASSED** |
| `exp-d2-wired2` → `exp-dA-gate2` | Task 3 (resend guard) | **PASSED** (after the fix below) |
| `exp-d2-wired2` → `exp-dA-gate2b` | Task 3, second sample | **PASSED** |
| `exp-d0-base2` → both gate runs | all three tasks | **PASSED**, 8 vs 16 server-runs |

The Task 2 isolation cost no extra run: `exp-d2-wired2` was taken with Tasks 1 and 2
deployed and Task 3 not yet built into the deployed binary, so it was already the right
point. Task 2 relocates 8 arrivals per run at 2 servers and perturbs neither the stable
counters nor conservation.

### Gate A first failed, and the failure was in the gate

`haloAhead`, pinned at 0 in `gate-compare.py`'s `STABLE` set, came back 1086.

**It was not the change under test.** `hoResent = 0` and `hoDup = 0` on every server of
every repeat: Task 3's guard only executes when a resend arrives, so the code path never
ran at all. The spike was confined to one repeat of four, where it co-occurred with
`haloLate` reaching 1989 and 250 on the two servers.

Two independent confirmations:

1. **It moves between repeats.** A second run of the same binary at the same configuration
   spiked `haloAhead` in a *different* repeat and at a different magnitude — r3/1086 first,
   r1/161 second — each paired with a `haloLate` spike on the other server (1989/250, then
   328/43). Intermittent, not deterministic.
2. **Two pre-change runs already disagree.** Gate-comparing `exp-d0-probe4u` against
   `exp-d1-clampcheck` — both taken *before* Task 3, differing only by a halo-neutral edit —
   fails on `haloSent`/`haloRecv` at 4 servers. This is the plan's own Task 4 Step 4
   diagnostic, and it settles the question without a new binary.

### The correction, and its blind spot

`haloAhead` is moved out of `STABLE` and range-checked, exactly as Phase A's ruling P11 did
for `haloLate`. **P11 fixed one face of a two-faced problem**: `haloAhead` and `haloLate`
are the same inter-server clock skew observed from the two ends of a link, so reclassifying
one while pinning the other at 0 could not hold. That it survived Phase A is because Phase A
never produced a run where the skew fell the other way.

A hazard note also records that `haloSent`/`haloRecv` are **not stable at 4 servers** — the
`STABLE` list was derived from Phase A's 2-server data and validated only there. Anyone
pointing the gate at a 4-server experiment must drop and range-check them, in the same
manner as the existing `objs` note.

**Blind spot, named rather than glossed:** the gate can no longer detect a change whose only
effect is on halo scheduling. That is narrow — no Phase D change touches halo scheduling, and
`hoResent = 0` independently establishes that Task 3 never executed on these runs — but it is
a real loss of coverage and it now applies to both faces of the counter rather than one.

`tools/test_gate_compare.py` and `tools/test_analyse.py` both still pass.

---

## Task 5 — the lookahead sweep

### Step 1: the halo bound on candidate lookaheads, computed before the sweep ran

Ruling D5: a handoff lookahead of `L` ticks means the sender keeps simulating the object for
`L` ticks *after* it has left the sender's region, so the receiver only sees it during that
window if it is inside the receiver's halo band:

```
v_max * L * dt <= halo_width
```

From `HaloBound.h`: `HALO_ASSUMED_MAX_SPEED = 60.0`, `HALO_ASSUMED_MAX_RADIUS = 2.0`,
`HALO_DEFAULT_SUBSTEP_HZ = 120`. At the measurement configuration's `--halo-width 8`:

```
L_max = halo_width * 120 / v_max = 8 * 120 / 60 = 16
```

So the swept range {0, 2, 4, 8, 16} has its top point **exactly at the limit**, with zero
margin: at L = 16 an object released late has travelled 0.5 * 16 = 8 units past the border,
landing precisely on the band edge. L = 8 travels 4 units and sits comfortably inside it.

L = 16 is kept in the sweep deliberately rather than trimmed. A point at the predicted
boundary is the one that tells you whether the boundary is real, and Phase C §4.5's lesson is
that a sweep which never samples the predicted point can only return all-pass or all-fail.
It is **not** a candidate for the default: a default with zero margin would fail on any
workload faster than `uniform`, and `headon` launches at the same 60 but head-on, halving the
time to contact.

Note this is a **separate** parameter from `--halo-lookahead` (default 4), whose own bound
`MinimumSafeHaloWidth(4) = 0.5 * 4 + 4 = 6` is satisfied by the configured width of 8. The two
lookaheads are deliberately independent; only the handoff one is swept here.

### Step 2: the sweep at 2 servers, 30 runs

`--workload uniform`, 400 objects, 1800 paced ticks, `--halo-width 8`, `--halo-reliable`,
30 s drain, **6 repeats per point**. Every repeat is shown; no medians (see the bimodality
finding above).

| L | `ownership_gap_ticks`, per repeat | `double` | `hoLate` | verdict |
|---|---|---|---|---|
| 0 | 1712, 87, 1755, 87, 1766, 1744 | 2 | 0 | FAILS |
| **2** | **1776, 1776, 1775, 1776, 1777, 1774** | **3** | **55** | **FAILS — worse than L = 0** |
| 4 | 0, 0, 0, 0, 0, 0 | 0 | 0 | **CLEAN** |
| 8 | 0, 0, 0, 0, 0, 0 | 0 | 0 | CLEAN |
| 16 | 0, 0, 0, 0, 0, 0 | 0 | 0 | CLEAN |

**L = 4 is the smallest lookahead that closes the gap at 2 servers**: zero on all six repeats,
no double-ownership, no late arrivals.

#### The finding this plan did not anticipate: a small lookahead is worse than none

L = 2 is not merely insufficient. It is worse than the current default. At L = 0 the gap is
bimodal — two repeats at 87, four at ~1750. At L = 2 **every** repeat sits at ~1776, it
produces the most double-ownership of any point (3), and it is the only point with late
arrivals.

The mechanism is measured, not inferred. Per-server `hoLate` at L = 2 reads **18 and 31** —
roughly 60% of the ~81 arrivals miss their scheduled slot — against **0** at both L = 4 and
L = 8. Two ticks at 120 Hz is 16.7 ms, which is below the delivery-plus-pacing latency of this
setup; four ticks (33 ms) is above it. When an arrival misses its slot the receiver applies it
immediately, but the sender has already released at its own scheduled tick, so the transfer
degrades to release-on-send *plus* a scheduling delay — strictly worse than release-on-send
alone.

**A precision note on `hoLate` at L = 0.** It reads 0 there, but that is structural, not a
sign of timeliness: `StartHandlingObject` returns through the `mHandoffLookaheadTicks <= 0`
branch before the late check is reached, so there is no scheduled slot to miss and the counter
is unreachable. L = 0's zero and L = 4's zero mean different things.

**Consequence.** The handoff lookahead is a **threshold** parameter with a wrong-side-of-it
failure, not a dial on which more is monotonically better. Below the delivery latency it adds
delay without buying atomicity. Neither `docs/superpowers/specs/2026-08-23-backlog-completion-design.md`
§5 nor the roadmap says this, and a deployment that set a small non-zero lookahead "to be safe"
would be worse off than leaving it at 0.

#### The bimodality is more common than the baseline suggested

At L = 0, **four of six** repeats landed in the high mode, where the 4-repeat baseline had
shown one of four. The high mode is the common case, not the outlier. Raising the sweep from 3
repeats to 6 was necessary: at 3 there was a real chance of drawing three low-mode runs and
under-stating the defect by an order of magnitude.

### Step 3: the sweep at 4 servers, 30 runs — and a conflict between two rulings

Same configuration, 4 servers, `--workload uniform`, 6 repeats per point.

| L | `ownership_gap_ticks`, per repeat | `double` | `hoLate` | verdict |
|---|---|---|---|---|
| 0 | 99, 102, 108, 109, 101, 105 | 0 | 0 | FAILS |
| 2 | 1777, 1777, 1776, 1775, 1775, 1775 | 3 | 63 | FAILS |
| 4 | 0, 0, 0, **26, 13, 43** | 0 | 6 | **FAILS — 3 of 6** |
| 8 | **10**, 0, 0, 0, 0, 0 | 0 | 3 | **FAILS — 1 of 6** |
| 16 | 0, 0, 0, 0, 0, 0 | 0 | 0 | **CLEAN** |

Two things differ from the 2-server sweep. The gap at L = 0 is **not bimodal** here — six
repeats at 99–109, tightly clustered — and L = 4, which was clean on all six at 2 servers,
fails half its repeats at 4.

#### Every remaining gap is a late arrival, one for one

The per-repeat correlation is exact:

| L | repeat | `hoLate` (per server) | gap |
|---|---|---|---|
| 4 | r1–r3 | 0, 0, 0 | 0 |
| 4 | r4 | 1+3+0+2 = 6 | 26 |
| 4 | r5 | 4+0+0+0 = 4 | 13 |
| 4 | r6 | 0+1+0+2 = 3 | 43 |
| 8 | r1 | 0+3+0+0 = 3 | 10 |
| 8 | r2–r6 | 0 | 0 |
| 16 | r1–r6 | 0 | 0 |

`hoLate > 0` ⟺ `gap > 0`, with no exceptions across 18 runs. Once the lookahead is above the
threshold, **the ownership gap is entirely explained by arrivals missing their scheduled slot** —
it is not a residual protocol defect but the tail of delivery latency exceeding the window.

That tail is a property of *this machine*. Four servers plus manager, midware and client is
seven processes on six cores; the same L = 4 that never produced a late arrival in 30 runs at 2
servers produces a handful at 4. The required lookahead is therefore set by scheduling
contention, not by the design.

#### The conflict

Ruling D4's criterion — the smallest L clean on **every** repeat at **both** server counts —
selects **L = 16**. Ruling D5's bound admits L ≤ 16 at `--halo-width 8`, so L = 16 is
admissible by the letter and has **zero margin**: a late-released object travels exactly
`0.5 × 16 = 8` units, landing precisely on the band edge. Step 1 ruled 16 out as a default in
advance for that reason, before any of this data existed.

So the two rulings select values that do not overlap. This is recorded rather than resolved
unilaterally, per Task 5 Step 4's instruction to stop and report rather than invent a
mechanism.
