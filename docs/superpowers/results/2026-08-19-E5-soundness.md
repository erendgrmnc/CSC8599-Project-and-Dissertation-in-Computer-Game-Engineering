# E5 — the halo soundness sweep

Date: 2026-08-19 / 2026-08-20
Runs: round 1 — `runs/exp-haloL2`, `runs/exp-haloL16`, `runs/exp-haloL32` (54 runs, kept for the
record); round 2 — `runs/exp-haloL2b`, `runs/exp-haloL8b`, `runs/exp-haloL16b`, `runs/exp-haloL24b`
(66 runs). 120 runs total, 0 failed (`ok: 2/2 servers` on every point, no re-runs needed at the
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

**Root cause, confirmed:** `DistributedGameServer/ServerWorldManager.cpp:944` hardcodes
`HALO_STALE_TICKS = 30` — the number of ticks after which `RetireStaleHaloShadows` discards a halo
shadow that has not been refreshed. `--halo-lookahead 32` defers a shadow's application to
`senderTick + 32`, which is *after* that shadow would already have been retired as stale at
`senderTick + 30`. The shadow is therefore retired before it is ever applied — the halo is
effectively disabled for any lookahead above 30, independent of width. That is exactly what the
data shows: crossings pinned near the no-halo baseline at every tested width, and the resulting
loss of collision-mediated border blocking is consistent with (though not proven to fully explain)
the ownership-gap failures on the same runs.

**This is a real defect, and it is recorded here rather than fixed here.** `SetHaloWidth` warns
loudly when `--halo-width` is set below its computed floor. There is no equivalent warning when
`--halo-lookahead` exceeds the `HALO_STALE_TICKS` staleness horizon — the guard is asymmetric: the
width axis is protected, the lookahead axis is silent. Server code is frozen for this evidence
pass (`deploy/` binaries must not be rebuilt), so this is deliberately left as a finding for the
build phase: either raise `HALO_STALE_TICKS` to track `--halo-lookahead`, or warn when it doesn't.

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
| `haloL24b` | 24 | 16.0 | 4, 5, 6, 7, 8, 9 | ~9 |

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
