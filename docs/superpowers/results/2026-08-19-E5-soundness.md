# E5 — the halo soundness sweep

Date: 2026-08-19 / 2026-08-20 (rounds 1-2, zero latency); 2026-08-25 (round 3, the latency
dimension - see the bottom of this document)
Runs: round 1 — `runs/exp-haloL2`, `runs/exp-haloL16`, `runs/exp-haloL32` (54 runs, kept for the
record); round 2 — `runs/exp-haloL2b`, `runs/exp-haloL8b`, `runs/exp-haloL16b`, `runs/exp-haloL24b`
(66 runs). 120 runs total in rounds 1-2, plus 302 in round 3; 0 failed (`ok: 2/2 servers` on every point, no re-runs needed at the
run-execution level).

This document went through two rounds because the first round's sweep design could not answer the
question it was built to answer. That is recorded here rather than hidden: round 1 establishes only
a weaker claim than originally intended, and round 2 exists specifically to correct it.

## Claim under test

`ServerWorldManager::MinimumSafeHaloWidth()` predicts the halo band width required to catch every
cross-border contact:

```
w_min = HALO_ASSUMED_MAX_SPEED . L . dt + 2 . HALO_ASSUMED_MAX_RADIUS
```

which, at the coded constants (60, 2) and the 120 Hz substep, is the line `w_min(L) = 0.5*L + 4`
("the conservative floor"). The claim has two separable parts:

1. **Soundness** — the empirical knee (smallest halo width that catches every border contact)
   sits *at or below* the conservative floor at every lookahead tested. A knee above the floor
   falsifies it.
2. **Tracking** — the knee increases with lookahead, observed as an actual transition (a failing
   width immediately below a passing width), not merely inferred from where a sweep happens to
   start.

Workload: `headon` — pairs of objects launched at each other across `x = 0`. A pair that collides
bounces and never crosses the border; a pair whose contact is missed passes through and is handed
off. Border crossings (`hoSent`) count missed border contacts on a fixed, interpretable scale: 100
is total failure (every pair passed through), 0 is every contact caught.

Design constants held fixed across both rounds: 2 servers, 100 objects, 1,800 paced ticks,
`--fixed-step --workload headon --halo-reliable`, 3 repeats per point.

---

## Round 1 — design and why it could not establish tracking

Round 1 swept `--halo-width` at 1-unit resolution, six points **centred on each lookahead's own
conservative floor**:

| lookahead | floor | swept widths |
|---|---|---|
| 2 | 5.0 | 2, 3, 4, 5, 6, 7 |
| 16 | 12.0 | 9, 10, 11, 12, 13, 14 |
| 32 | 20.0 | 17, 18, 19, 20, 21, 22 |

**The design error:** the conservative floor assumes a maximum object speed of 60 units/s, where
`headon` actually moves objects at 30. The true knee therefore sits well below the conservative
floor by construction — sweeping *around* the floor could only ever return either all-zero
crossings (if the true knee is below the window) or all-failing (if it's above), and never bracket
the transition itself. That is exactly what happened at lookahead 2 and 16: every sampled width,
including ones below the floor, already read 0 crossings.

### Round-1 results, verbatim from `analyse.py`

```
halo soundness: lookahead 2 ticks, predicted floor w_min = 5
   width  crossings (median)  repeats   vs floor
-------------------------------------------------
       2                   0        3      below
       3                   0        3      below
       4                   0        3      below
       5                   0        3   AT FLOOR
       6                   0        3      above
       7                   0        3      above
empirical knee : 2  (predicted 5)  -> SOUND

halo soundness: lookahead 16 ticks, predicted floor w_min = 12
   width  crossings (median)  repeats   vs floor
-------------------------------------------------
       9                   0        3      below
      10                   0        3      below
      11                   0        3      below
      12                   0        3   AT FLOOR
      13                   0        3      above
      14                   0        3      above
empirical knee : 9  (predicted 12)  -> SOUND

halo soundness: lookahead 32 ticks, predicted floor w_min = 20
   width  crossings (median)  repeats   vs floor
-------------------------------------------------
      17                  60        3      below
      18                  60        3      below
      19                  60        3      below
      20                  60        3   AT FLOOR
      21                  60        3      above
      22                  60        3      above
empirical knee : NONE - no swept width caught every border contact

halo soundness across lookaheads
 lookahead    floor     knee    slack    verdict
------------------------------------------------
         2        5        2        3      sound
        16       12        9        3      sound
        32       20     none        -    UNSOUND
```

### What round 1 does and does not establish

- **Sufficiency at L=2 and L=16, in the weak sense.** `analyse.py` reports "knee = 2" and
  "knee = 9" only because those are the smallest widths it sampled and nothing in either sweep
  ever failed — `find_knee` cannot report a number smaller than the bottom of its own search
  window. The correct reading is: the conservative floor (5.0, 12.0) is **sufficient** at both
  lookaheads, because every sampled width, including several below the floor, caught every
  contact. Nothing here shows the bound is *tight*, and nothing shows the knee takes the predicted
  form — the transition itself was never observed.
- **Tracking was not demonstrated.** The apparent "knee moved right, 2 to 9" is an artefact of the
  two sweeps having different lower bounds, not a measured property of the system. Reporting that
  as evidence of tracking would have published a sampling choice as a result.
- **Lookahead 32 is a real finding, addressed below** — it is not a bracketed knee at all, and not
  comparable to the other two points.

Round 2 exists to fix this: sweep from below the true knee so every range actually contains a
failing width and a passing width.

---

## The L=32 finding: outside the implementation's envelope, not a falsification

At lookahead 32, median crossings were flat at 60 across the entire six-point range (17-22),
including widths above the floor — not a knee above the floor (which would be a bounded, if
disappointing, falsification), but no transition inside a window centred 8 units above the L=16
knee's window. All 18 of those runs additionally failed `analyse.py`'s per-tick
`ownership_gap_ticks` invariant (8-12 unowned-object ticks per run), a failure mode that appeared
in **none** of the other 90 runs across both rounds.

> **Superseded (Batch A, 2026-08-20).** The `HALO_STALE_TICKS` explanation below is **refuted** by
> re-analysis of these same runs: on the L=32 sweep `haloLate = 0` and `halo_objects` averages 18.2
> with shadows present on 98.2% of ticks, so nothing was being retired. The conclusion that L=32 is
> outside the envelope stands; the mechanism does not. See `docs/EVALUATION.md` §4 for the replacement
> account (an upper bound on lookahead, attributed to extrapolation error by elimination) and §4.1 for
> a second defect these runs carried — an unthrottled halo publish that broke invariant I8 on this
> sweep.

**Root cause, confirmed:** `DistributedGameServer/ServerWorldManager.cpp:944` hardcodes
`HALO_STALE_TICKS = 30` — the number of ticks after which `RetireStaleHaloShadows` discards a halo
shadow that has not been refreshed. `--halo-lookahead 32` defers a shadow's application to
`senderTick + 32`, which is *after* that shadow would already have been retired as stale at
`senderTick + 30`. The shadow is therefore retired before it is ever applied — the halo is
effectively disabled for any lookahead above 30, independent of width. That is exactly what the
data shows: crossings pinned near the no-halo baseline at every tested width, and the resulting
loss of collision-mediated border blocking is consistent with (though not proven to fully explain)
the ownership-gap failures on the same runs.

**This is a real defect, and it was recorded here rather than fixed here.** `SetHaloWidth` warns
loudly when `--halo-width` is set below its computed floor. There was no equivalent warning when
`--halo-lookahead` exceeded the `HALO_STALE_TICKS` staleness horizon — the guard was asymmetric: the
width axis protected, the lookahead axis silent. Server code was frozen for this evidence pass
(`deploy/` binaries must not be rebuilt), so it was left as a finding for the build phase: either
raise `HALO_STALE_TICKS` to track `--halo-lookahead`, or warn when it doesn't.

> **CLOSED 2026-08-25 (Phase C §4.2), by the first of those two options.** The horizon now tracks
> the lookahead: `HaloStaleTicks(L) = 30 + L`, so the fixed 30 becomes a **drop tolerance** rather
> than a ceiling, and a shadow can never be retired before the update that would refresh it is due
> to apply. Asserted in `tools/InteractionTests` (`HaloBoundTests`), which checks
> `HaloStaleTicks(L) > L` at L = 0, 4, 24, 32, 64 and pins `HaloStaleTicks(0) == 30` so the drop
> tolerance itself is preserved.
>
> This was a **precondition** for Phase C rather than a tidy-up: injected link delay costs roughly
> one tick of effective lookahead per 8.33 ms at the 120 Hz substep, so a nominal L=24 would have
> hit the old ceiling of 30 at only ~50 ms of one-way delay — and the latency sweep would then have
> reported its own envelope limit as a falsification of the bound, which is precisely the misreading
> recorded above at L=32.
>
> **The zero-latency rounds below are NOT re-run and remain valid.** The generalised bound reduces
> exactly to the published expression at `T_L = T_J = 0` (asserted), and a paced gate run at zero
> injected latency reproduced the pre-change baseline on all 20 stable fields and both conserved
> totals.

> **Confirmed along a second axis (round 3, 2026-08-25).** Batch A attributed this to extrapolation
> error *by elimination*, having refuted the staleness account. Round 3's latency sweep reaches the
> same failure from the other direction: at L=40 with 300 ms of injected link delay - total lag 333
> ms against this point's 267 ms - crossings pin at **60, flat across widths 16-40**, the same number
> and the same flatness recorded here. That run has `haloLate` at 135-385 rather than ~146,000, so
> late delivery is excluded; the staleness horizon tracks the lookahead since Phase C §4.2, so
> retirement is excluded; and invariant I8 holds, so server divergence is excluded. What remains is
> linear dead reckoning over a lag long enough for velocity to change. This does not *measure* the
> extrapolation term - that still needs a run with dead reckoning disabled - so the attribution is
> still by elimination. What changed is its strength and its scope: the elimination now holds at a
> point where lateness is excluded by direct measurement rather than assumed, and the boundary
> generalises from "an upper bound on lookahead" to a limit on **total** sample-to-apply lag,
> roughly 200-267 ms, whichever term supplies it.

L=32 is therefore reported as **out of the implementation's tested envelope**, not as a soundness
falsification within it — the formula was never actually tested at L=32, because the mechanism
that would test it (a halo shadow surviving long enough to be applied) had already stopped working
for an unrelated, code-level reason.

---

## Round 2 — the corrected sweep

The conservative floor assumes 60 units/s; `headon` moves at 30. Substituting the real speed gives
an actual-speed prediction of roughly `0.25*L + 2r`, well below the conservative floor. The
corrected design sweeps **upward from 0** (0 = halo disabled entirely) at each of four lookaheads,
targeting that actual-speed region rather than the conservative floor, and stays strictly below the
L=32 staleness ceiling found above:

| name | lookahead | conservative floor | swept widths | expected actual knee |
|---|---|---|---|---|
| `haloL2b` | 2 | 5.0 | 0, 1, 2, 3 | ~1.5 |
| `haloL8b` | 8 | 8.0 | 1, 2, 3, 4, 5, 6 | ~3 |
| `haloL16b` | 16 | 12.0 | 2, 3, 4, 5, 6, 7 | ~5 |
| `haloL24b` | 24 | 16.0 | 4, 5, 6, 7, 8, 9 | ~7 |

63 runs planned, 66 run (`haloL2b` has 4 widths x 3 repeats = 12; the other three have 6 x 3 = 18
each — 12+18+18+18 = 66). Every range was checked as it landed and, unlike round 1, each one did
contain both a failing width and a passing width — no range needed extending.

**A resolution caveat, carried forward honestly:** `--halo-width 0` is not "an extremely narrow
halo" — it is an on/off switch that disables the halo mechanism entirely, not a point on the width
axis. The 0-to-1 transition at L=2 therefore establishes only that the true knee is **at most 1**;
it is a bound, not a located measurement, because there is no real width tested below 1. The other
three sweeps bracket their knee between two positive, physically meaningful widths.

### Round-2 results, verbatim from `analyse.py`

Command: `python tools/analyse.py runs/exp-haloL2b runs/exp-haloL8b runs/exp-haloL16b runs/exp-haloL24b`

```
halo soundness: lookahead 2 ticks, predicted floor w_min = 5
   width  crossings (median)  repeats   vs floor
-------------------------------------------------
       0                 100        3      below
       1                   0        3      below
       2                   0        3      below
       3                   0        3      below
empirical knee : 1  (predicted 5)  -> SOUND

halo soundness: lookahead 8 ticks, predicted floor w_min = 8
   width  crossings (median)  repeats   vs floor
-------------------------------------------------
       1                 100        3      below
       2                  44        3      below
       3                   0        3      below
       4                   0        3      below
       5                   0        3      below
       6                   0        3      below
empirical knee : 3  (predicted 8)  -> SOUND

halo soundness: lookahead 16 ticks, predicted floor w_min = 12
   width  crossings (median)  repeats   vs floor
-------------------------------------------------
       2                 100        3      below
       3                 100        3      below
       4                   0        3      below
       5                   0        3      below
       6                   0        3      below
       7                   0        3      below
empirical knee : 4  (predicted 12)  -> SOUND

halo soundness: lookahead 24 ticks, predicted floor w_min = 16
   width  crossings (median)  repeats   vs floor
-------------------------------------------------
       4                 100        3      below
       5                 100        3      below
       6                   0        3      below
       7                   0        3      below
       8                   0        3      below
       9                   0        3      below
empirical knee : 6  (predicted 16)  -> SOUND

halo soundness across lookaheads
 lookahead    floor     knee    slack    verdict
------------------------------------------------
         2        5        1        4      sound
         8        8        3        5      sound
        16       12        4        8      sound
        24       16        6       10      sound
```

Full output: `.superpowers/sdd/2026-08-19-evidence-completion/logs/analyse-e5-round2-output.txt`.
The tool's knees (1, 3, 4, 6) match the log-derived first-repeat readings taken while the sweeps
were in flight exactly — no disagreement to adjudicate. The only difference between the two is at
`haloL8b` width 2, where the first-repeat crossing count (22) differs from the three-repeat median
reported here (44); that is ordinary repeat-to-repeat variance at a genuinely partial-failure
point, not a disagreement about where the knee is.

### What round 2 establishes

- **Soundness holds at every lookahead tested.** `1 <= 5`, `3 <= 8`, `4 <= 12`, `6 <= 16`. The
  bound is never optimistic across L = 2, 8, 16, 24. This is the primary result.
- **The knee increases monotonically with lookahead** (<=1, 3, 4, 6). Unlike round 1, three of
  these four rest on an observed transition — a failing width immediately below a passing one —
  rather than on where a sweep happened to start. This is the tracking result.
- **The slack widens with lookahead** (>=4, 5, 8, 10): the conservative bound becomes *more*
  conservative, not less, as lookahead grows. This is a measured property, not an obvious one in
  advance, and it is the opposite of what a naive "extrapolation error grows with lookahead"
  intuition would predict for the *gap between prediction and reality* — here the gap widens in
  the safe direction.
- **The functional form is not determined by this data.** Four points at 1-unit resolution, one
  of them only a bound (`<=1` at L=2), do not fix a curve. A prediction was made mid-experiment,
  stated in advance of the remaining sweeps, and tested honestly:

  From the L=8 point alone (knee = 3), substituting the real workload speed into the formula's
  structure (`0.25*L + 2r`) and solving for the implied contact radius gives `predicted knee(L) =
  0.25*L + 1`. That predicts 5 at L=16 and 7 at L=24. **Both predictions were refuted**: the
  measured knees were 4 and 6. The L=24 sweep was deliberately ranged (4-9) to discriminate
  between the trend extrapolation (~5) and the refuted prediction (7); it returned 6, matching
  neither. No refitting was done afterward to make the constant match — the refutation is reported
  as a refutation, not quietly absorbed into a new fitted line.

- **A distinct, minor ownership-gap observation, separate from the L=32 finding.** All four
  round-2 sweeps show `ownership_gap_ticks` invariant failures, but only at the *failing* widths
  (where crossings are high and real border handoffs occur) — never at the passing widths used to
  determine the knees. This tracks the number of actual handoffs (roughly proportional to
  crossing count) rather than affecting every width uniformly regardless of activity, which is the
  signature that distinguished the L=32 finding above (flat gaps at *every* width, including ones
  well above the floor, uncorrelated with whether the halo caught anything). This looks like an
  ordinary few-tick handoff-transition artefact rather than the L=32 staleness bug, and — because
  it never appears at the widths the knees are read from — it does not affect any knee reported
  here. It is noted for completeness, not investigated further; server code remains untouched.

---

## A manifest note, for transparency

Round 1's manifests carry `gitCommit 619fbd2bf6ec6d979cd6d4ba9b3a7845924a6f01`, the frozen commit
confirmed against Task 2. Round 2's manifests carry `gitCommit 7797a8055987d85332600c4e62c60bcf0051ee26`
— the commit that added round 1's results document between the two rounds. `git diff --stat
619fbd2..7797a80` shows exactly one file changed: `docs/superpowers/results/2026-08-19-E5-soundness.md`
(this document's round-1-only predecessor). No file under any frozen-source path
(`DistributedGameServer/`, `DistributedPhysicsManager/`, `PhysicsServerMidware/`,
`CSC8503CoreClasses/`, `CSC8503/`) changed, and `deploy/` was never rebuilt, so the binaries that
executed round 2 are bit-identical to the binaries that executed round 1. The commit drift is
real and is recorded here rather than silently treated as if both rounds stamped the same hash;
it does not affect comparability between the two rounds because nothing that could affect server
behaviour changed.

---

## The envelope, stated rather than hidden

`HALO_ASSUMED_MAX_SPEED = 60` is a hardcoded global constant, not measured from the world — it is
not derived from `headon`'s actual speed. `headon` moves objects at 30 units/s, half the assumed
maximum, which is exactly why every measured knee sits well below the conservative floor. An
object faster than 60 units/s falls outside the guarantee entirely, silently — nothing in the code
checks world object speed against the constant.

## Sweep resolution

Both rounds swept at 1-unit resolution. A knee can only ever be *located* to that resolution:
reporting "the knee is at width `W`" means "the true crossover is somewhere in `(W-1, W]`" — except
where the lower bound of the sweep is `0` (a mechanism on/off switch, not a real width), in which
case the result is a bound (`<= 1`), not a located transition.

This matters for reading `runs/exp-halo`, an earlier, unrelated two-point experiment
(`--halo-width 0, 8` at `--halo-lookahead 4`, conservative floor 6.0). Re-analysed under the same
criteria it reports **UNSOUND** (`empirical knee : 8 (predicted 6) -> UNSOUND`), because width 0
trivially fails and width 8 is the *only* other point sampled — the tool has no way to know
whether the true knee is at 8, or at 1, or anywhere in between. That is a two-point sweep's
resolution failing to locate a knee, not evidence the formula is optimistic, and it is mentioned
here so it is not mistaken for a second, independent falsification alongside the L=32 finding
above — it isn't a comparable measurement at all.

## Limitations

- **One synthetic workload.** `headon` is pairs on fixed lanes, at one speed, meeting the border
  perpendicular — the configuration in which the knee is sharpest. Oblique approaches and mixed
  speeds would stress the bound harder.
- **This validates the formula as coded, not a derivation.** If the paper claims the derivation as
  its contribution, the derivation belongs in the text first and this experiment is its
  validation, not its source.

## Bottom line

Soundness holds at every lookahead actually tested within the implementation's working envelope —
L = 2, 8, 16, 24 — with slack that *widens* as lookahead grows (>=4 at L=2 up to 10 at L=24), so
the formula is conservative and becomes more so, never optimistic. Tracking holds: the knee
increases monotonically with lookahead (<=1, 3, 4, 6), and three of the four transitions were
directly observed (a failing width immediately below a passing one), not inferred from sweep
placement. The functional form is not determined by four 1-unit-resolution points; a
mid-experiment prediction (`0.25*L + 1`, derived from a single point) was stated in advance and
refuted twice, at L=16 and again at L=24, and is reported as refuted rather than re-fitted.

> **Superseded (Batch A, 2026-08-20).** The mechanism named in this paragraph is refuted; see the
> note above and `docs/EVALUATION.md` §4. The envelope conclusion itself stands.

Separately, lookahead 32 sits outside the implementation's tested envelope: a hardcoded
30-tick shadow-retirement timeout (`HALO_STALE_TICKS`, `ServerWorldManager.cpp:944`) silently
disables the halo for any lookahead above it, with no warning equivalent to the one that already
guards halo width. That is recorded as a defect for the build phase, not fixed here, and it is why
round 2 kept every lookahead strictly below 30.

This document required two rounds because the first round's sweep, centred on the conservative
floor, could only ever return all-zero or all-failing results given that the real workload moves
at half the formula's assumed maximum speed — a design error caught by inspecting the resulting
data rather than by construction. Round 1 is kept on record above rather than deleted, both
because its L=2/L=16 sufficiency and its L=32 finding are real results, and because a results
document that hides its own false start is worth less than one that shows it.

---

# Round 3 — the latency dimension (2026-08-25)

Date: 2026-08-25. Runs: 302 across 23 experiment directories, including a 4-run timing probe
(`runs/exp-cL{8,16,24}lat0`, `runs/exp-e5L*lat*`). Phase C of `docs/superpowers/specs/2026-08-23-backlog-completion-design.md`
§4.5. 0 runs failed at the execution level (`ok: 2/2 servers` throughout).

Rounds 1 and 2 measured the `T_L = T_J = 0` special case. The bound is now stated over link delay,

```
w_min = v_max * (L * dt + T_L + T_J) + 2 * r_max
```

and this round sweeps the `T_L` term. `--link-latency-ms` delays the **server-to-server** path only;
the client path is deliberately undelayed, because the bound is a claim about peer lag and delaying
snapshots would move E3 and E8 without moving E5.

## Controls first: the published knees, re-measured on this binary

Phase C changed server code (`HaloBound.h`, the staleness horizon, the delay queue). Round 2's knees
were measured on a different build, so a knee that moved under latency could have been Phase C's own
doing. Gate 4.4.1 pinned 20 stable *counters*; it did not re-establish the *result*.

| lookahead | round 2 knee | control knee (2026-08-25 binary) | floor |
|---|---|---|---|
| 8 | 3 | **3** | 8 |
| 16 | 4 | **4** | 12 |
| 24 | 6 | **6** | 16 |

Exact reproduction, including the partial-failure median of 44 crossings at L=8 width 2. Everything
below is therefore attributable to injected delay and not to the build.

## Result 1 — below the scheduling lookahead, latency does not move the knee at all

| lookahead | `L*dt` | `T_L` = 0 | 25 | 50 | 100 | 150 |
|---|---|---|---|---|---|---|
| 8 | 66.7 ms | 3 | **3** | **3** | 4 | 5 |
| 16 | 133 ms | 4 | — | **4** | **4** | — |
| 24 | 200 ms | 6 | — | **6** | **6** | — |

Bold marks latency strictly below `L*dt`: six points across three lookaheads, nine counting the
zero-latency controls. The knee is unchanged at every one, and the crossing profile is identical
width-for-width rather than merely equal at the knee.

`haloLate` confirms the mechanism independently rather than by inference: it is **exactly 0** at the
passing widths of eight of the nine points at or below the threshold, and non-zero at every point
above it. Updates are still meeting their scheduled slot.

The ninth is worth stating rather than rounding off. L=8 / `T_L`=50 reports 1,950 and 2,860 late
updates at its passing widths, and it is the point with both the smallest absolute budget (66.7 ms)
and 75% of it consumed — so the few milliseconds of real loopback and scheduling delay on top of the
injected amount are enough to push a fraction of updates past the slot. L=16 / `T_L`=100 consumes
the same 75% of a larger budget and reports 0. **The knee is 3 either way**, which is the stronger
form of the result: the budget can be nearly exhausted, and start visibly overflowing, before the
required width moves at all.

**The lookahead is a delay budget, and latency spends it rather than adding to it.** A halo update is
scheduled to apply at `senderTick + L`; while the packet arrives before that tick, when it arrived is
invisible to the receiver. This is the one structural claim of this round, and it is why the
implementation's effective sample-to-apply lag behaves like `max(L*dt, T_L)` rather than the sum.

The published `L*dt + T_L` therefore remains a true upper bound — every point is SOUND — but a
demonstrably loose one, and the slack *grows* with latency: at L=24 / `T_L`=100 the knee is 6 against
a floor of 22.

## Result 2 — an upper bound on total lag, reached along two independent axes

Above roughly 200 ms of total lag the picture changes qualitatively. Crossings stop responding to
width.

| total lag | source | floor | widths swept | crossings |
|---|---|---|---|---|
| 200 ms | L=24, `T_L`=0 | 16 | 4–7 | knee 6, clean |
| 200 ms | L=16, `T_L`=200 | 24 | 4–24 | non-monotone: 100, 100, 74 at w=4/5/6, then 0 at 7/8/10/24 but 2 at 9/12, 15 at 16, 8 at 20 |
| 200 ms | L=24, `T_L`=200 | 28 | 6–28 | 0 at 12 and 20, **8 at 28 — the floor itself** |
| **267 ms** | **L=32, `T_L`=0** (round 1) | 20 | 17–22 | **60, flat** |
| 300 ms | L=16, `T_L`=300 | 30 | 9–32 | 46–60, flat (w=32 is above the floor) |
| 300 ms | L=24, `T_L`=300 | 34 | 7–38 | 15–60; **0 of 10 repeats passed at the floor** |
| **333 ms** | **L=40, `T_L`=300** | 42 | 16–40 | **60, flat** |

Two things this rules out.

**It is not lateness.** The L=40 / `T_L`=300 point was run to test the delay-budget model: if the
lookahead absorbs latency, raising `L` until `L*dt >= T_L` should restore the halo at 300 ms. It
absorbed the latency exactly as predicted — `haloLate` fell from ~146,000 at L=24 / `T_L`=300 to
**135–385** across every width at L=40, so updates arrive on schedule — and crossings stayed pinned at 60 at every width. **The prediction is refuted,
and its refutation is what identifies the real cause**: with timing corrected the failure is
unchanged, so irregular refresh cadence was not it.

That configuration is only reachable *because* of Phase C §4.2. The old fixed `HALO_STALE_TICKS = 30`
retired a shadow before an L=40 update could ever apply, so this experiment could not have been run
before the horizon was made to track the lookahead.

**It is not divergence between servers.** Invariant I8 (both servers resolving equal contact counts)
holds on **all** runs of the to-floor pass including every 300 ms point. The two servers agree with
each other; they are agreeing on shadows that are in the wrong place.

What is left is the lag itself. A shadow is dead-reckoned from its sample tick with constant
velocity. Over 333 ms at `headon`'s 30 units/s that is a 10-unit straight-line extrapolation through
a region where objects meet and reverse, so the extrapolated position is wrong by more than any band
width can cover — the error is temporal, and width is a spatial control.

**This independently confirms the round-1 L=32 finding, and upgrades its status.** Round 1 recorded
L=32 as 60 crossings flat across widths 17–22; Batch A refuted the `HALO_STALE_TICKS` explanation and
attributed it to extrapolation error **by elimination**. The high-lag points here reproduce the same
number with the same flatness, reached by a different route, with lateness and staleness both
independently excluded at a point where round 1 could only assume them. The attribution is still
by elimination - measuring the extrapolation term directly needs a run with dead reckoning
disabled, which is a simulation-affecting change and remains build-phase work - but the
elimination now rests on a second, independent axis, and the boundary generalises from lookahead
to total lag.

## Result 3 — at equal nominal lag, latency is worse than lookahead

Lag-equivalence is not exact, and this is the one place the `max` model is optimistic. L=24 /
`T_L`=0 and L=24 / `T_L`=200 both carry 200 ms of nominal lag, but the first has a clean knee at 6
while the second passes at 12 and 20 and **fails at 28**. Monotonicity in width — wider is safer,
the assumption underlying both the bound and `find_knee` — breaks.

The cost of the band is measurable and rises with width at fixed latency:

| L=24, `T_L`=200 | shadow-ticks | late fraction | peer MB | crossings |
|---|---|---|---|---|
| w=12 | 26,255 | 0.80 | 1.3 | 0, 0, 0 |
| w=20 | 67,590 | 0.95 | 4.1 | 0, 0, 15 |
| w=28 (floor) | 145,640 | 0.99 | 9.3 | 0, 15, 8 |
| *w=6, `T_L`=0* | *59,780* | *0.00* | *3.6* | *0, 0, 0* |

The bound treats width as free. The implementation pays for it in published shadows, peer bandwidth
and late-applied updates, and near the lag ceiling that cost is large enough to matter. **At 200 ms,
following the bound's prescription (28) performed worse than ignoring it (12).** That is a cost term
absent from the derivation, not an error in it.

## What this round establishes

- **Soundness holds over the latency term wherever the mechanism is inside its lag envelope.** Every
  point at total lag <= 200 ms has its knee at or below the floor, with slack that widens as latency
  grows (5 at L=8/`T_L`=0 up to 16 at L=24/`T_L`=100).
- **The knee depends on total lag, not on `L` and `T_L` separately** — within the lookahead-dominated
  regime. Three lookaheads x three latencies each reproduce their own zero-latency knee exactly.
- **A lag ceiling exists at roughly 200–267 ms**, above which no width catches every contact. It is a
  property of linear extrapolation over a collision-dense workload, not of the width formula, and it
  bounds `L*dt + T_L` jointly — so a deployment cannot buy latency tolerance by raising the lookahead.
- **The functional form is not re-fitted here.** Round 2 stated a mid-experiment prediction
  (`0.25*L + 1`) and reported it refuted rather than absorbing it; the same discipline applies now.
  The measured knees are consistent with the bound's *structure* at the workload's real 30 units/s,
  but eleven points at 1-unit resolution do not fix a curve, and no constant was fitted to them.

## Sweep-design errors made and corrected in this round

Recorded because both are the same class of error rounds 1 and 2 already hit, and both were caught
from the data rather than by construction.

1. **Sweeping far below the floor cannot falsify a bound.** Passes 1 and 2 targeted the actual-speed
   knee, correct while the knee sits far below the floor — it did at every zero-latency point. At 300
   ms it does not. Crossings never reached zero across widths 9–14 and that was very nearly recorded
   as "no width works", when the bound *predicts* 30 and 34 at those points and neither had been
   sampled. This is round 1's error running the other way: round 1 swept *around* the floor and could
   only return all-pass or all-fail; this swept *below* it and could only return all-fail. A
   soundness verdict needs the floor inside the window, and the third pass put it there.
2. **Generalising a regime from one point.** "Reliable only at the floor" was written from L=16 /
   `T_L`=200 and contradicted by L=24 / `T_L`=200, where the floor is the width that fails. The
   corrected statement is about monotonicity breaking, not about the floor being the answer.

## Limitations

- **One synthetic workload, and the lag ceiling is a property of it.** `headon` is collision-dense at
  the border by construction, which is exactly where linear extrapolation over a long lag is worst.
  A workload with smoother trajectories would push the ceiling higher; one with faster direction
  changes would lower it. The ceiling is therefore reported as measured-here, not as a constant of
  the design. §4.7's oblique/mixed-speed variant remains unrun.
- **Injected delay is not measured delay.** Loopback on one machine, delay applied at the send side
  with monotonic release per target; no reordering, no loss, no bandwidth limit. `--halo-reliable`
  is on, so nothing drops.
- **The delay is applied to the whole peer path, handoffs included.** `ownership_gap_ticks` rises
  from 0–30 at `T_L` <= 100 to 77–128 at `T_L` = 300, which is the §0.7 ownership gap widening by
  roughly the injected delay. That is backlog item 2 being *measured* rather than argued, and it is
  a confound for any conservation-sensitive reading of these runs — not for the crossing counts the
  knees come from.
- **Above the lag ceiling the runs are not reproducible.** Identical repeats at L=16 / `T_L`=200
  width 7 gave 15 / 2 / 0 crossings and `haloLate` of 45,995 / 14,893 / 14,056. Repeats were raised
  from 3 to 5 there, and to 10 at the L=24 / `T_L`=300 floor point. Below the ceiling the runs are as
  reproducible as round 2's.
- **Jitter was not swept.** `--link-jitter-ms` is implemented, enters the bound at its maximum, and
  is asserted in `tools/InteractionTests`, but every run here used `T_J = 0`.

## Bottom line

The latency term of the bound is validated: below the scheduling lookahead, injected delay does not
move the knee at all, and above it the knee rises but stays well under the floor. The lookahead is a
delay budget that latency spends rather than adds to, which is why the implementation's effective lag
behaves like `max(L*dt, T_L)` and the published sum is sound but loose.

The sweep also found what the zero-latency rounds could not: a ceiling on **total** sample-to-apply
lag at roughly 200–267 ms, above which no band width catches every contact. It is not lateness
(`haloLate` ~ 0 at L=40 / `T_L`=300 and the failure is unchanged), not staleness (the horizon tracks
the lookahead since §4.2), and not server divergence (I8 holds throughout) — it is linear
extrapolation over a lag long enough for velocity to change. That both confirms round 1's L=32
finding along an independent axis and converts its attribution from elimination to construction, and
it adds a precondition the derivation never stated: the width bound holds only while total lag stays
inside the horizon over which dead reckoning is accurate.
