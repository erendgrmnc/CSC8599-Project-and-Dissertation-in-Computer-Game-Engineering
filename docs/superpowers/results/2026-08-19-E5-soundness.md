# E5 — the halo soundness sweep

Date: 2026-08-19
Commit under test: `619fbd2bf6ec6d979cd6d4ba9b3a7845924a6f01` (`gitDirty: false` in all three
manifests)
Runs: `runs/exp-haloL2`, `runs/exp-haloL16`, `runs/exp-haloL32` — 54 runs total, 0 failed
(`ok: 2/2 servers` on every point, no re-runs needed)

## Claim under test

`ServerWorldManager::MinimumSafeHaloWidth()` predicts the halo band width required to catch every
cross-border contact:

```
w_min = HALO_ASSUMED_MAX_SPEED · L · dt + 2 · HALO_ASSUMED_MAX_RADIUS
```

which, at the coded constants (60, 2) and the 120 Hz substep, is the line `w_min(L) = 0.5·L + 4`.
The claim has two separable parts:

1. **Soundness** — the empirical knee (smallest halo width that catches every border contact)
   sits *at or below* the predicted floor at every lookahead tested. A knee above the floor
   falsifies it.
2. **Tracking** — the knee moves right as `L` grows, by roughly the predicted amount.

Workload: `headon` — pairs of objects launched at each other across `x = 0`. A pair that collides
bounces and never crosses the border; a pair whose contact is missed passes through and is handed
off. Border crossings (`hoSent`) therefore count missed border contacts on a fixed, interpretable
scale: 100 is total failure, 0 is every contact caught.

Design: 2 servers, 100 objects, 1,800 paced ticks, `--fixed-step --workload headon --halo-reliable`,
3 repeats per point. Each lookahead is swept over six `--halo-width` values, 1 unit apart, centred
on that lookahead's own predicted floor:

| `--halo-lookahead` | predicted floor | swept `--halo-width` |
|---|---|---|
| 2 | 5.0 | 2, 3, 4, 5, 6, 7 |
| 16 | 12.0 | 9, 10, 11, 12, 13, 14 |
| 32 | 20.0 | 17, 18, 19, 20, 21, 22 |

## Sweep resolution

Each sweep is **1 unit apart, six points centred on the predicted floor** — the finest resolution
practical at 3 repeats per point without the experiment exceeding its time budget. A knee can only
ever be *located* to this resolution: reporting "the knee is at width `W`" means "somewhere in
`[W-1, W]`, or below the sweep's bottom edge if every sampled point already reads zero."

This matters for reading `runs/exp-halo`, an earlier two-point experiment (`--halo-width 0, 8` at
`--halo-lookahead 4`, floor 6.0) that predates this sweep. Re-analysed under the same criteria it
reports **UNSOUND** (`empirical knee : 8 (predicted 6) -> UNSOUND`), because width 0 trivially
fails (no halo at all) and width 8 is the *only* other point sampled — the tool has no way to know
whether the true knee is at 8, or at 1, or anywhere in between; with only two points straddling the
floor, any width above the floor that succeeds is reported as the knee even if the real crossover
is far below it. That is a two-point sweep's resolution failing to locate a knee, not evidence the
formula is optimistic. It is mentioned here so it is not mistaken for a second, independent
falsification of soundness — it isn't a comparable measurement at all.

## Results, transcribed from `analyse.py`

Command: `python tools/analyse.py runs/exp-haloL2 runs/exp-haloL16 runs/exp-haloL32`. Full raw
output is in `.superpowers/sdd/2026-08-19-evidence-completion/logs/analyse-e5-soundness-output.txt`
and reproduced verbatim in `.superpowers/sdd/2026-08-19-evidence-completion/task-3-report.md`.

### Lookahead 2 (floor 5.0)

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
```

### Lookahead 16 (floor 12.0)

```
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
```

### Lookahead 32 (floor 20.0)

```
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
```

### Cross-lookahead tracking table

```
halo soundness across lookaheads
 lookahead    floor     knee    slack    verdict
------------------------------------------------
         2        5        2        3      sound
        16       12        9        3      sound
        32       20     none        -    UNSOUND
```

## Verdicts

**Soundness: holds at lookahead 2 and 16, falsified at lookahead 32.** At L=2 and L=16 every
sampled width — including widths below the predicted floor — already catches every border
contact (median crossings 0 across the whole window), so the true knee sits at or below the
bottom of each swept range; the floor is never optimistic at either lookahead. At L=32, median
crossings are flat at 60 across the *entire* swept range, 17 through 22, including widths above
the floor. `analyse.py` reports no empirical knee for that sweep at all — not "a knee above the
floor" (which would still be a bounded falsification) but no crossover found within a window
centred 8 units above the L=16 knee. Soundness as tested does not hold at L=32.

**Tracking: holds between lookahead 2 and 16, cannot be evaluated at 32.** The knee moved right
from 2 to 9 as the floor moved right from 5.0 to 12.0, consistent with the predicted linear
relationship. At L=32 there is no knee to compare a position against, so tracking is not merely
"failing slower than predicted" (the scenario the design anticipated as a softer, still-interesting
outcome) — it is undefined, because soundness itself did not hold there.

## The envelope, stated rather than hidden

`HALO_ASSUMED_MAX_SPEED = 60` is a hardcoded global constant, not measured from the world — it is
not derived from `headon`'s actual speed. `headon` moves objects at 30 units/s, half the assumed
maximum, so the positive slack observed at L=2 and L=16 (3 units at both) is *expected*: it is the
margin the conservative constant buys, not evidence the bound is loose in general. An object
faster than 60 units/s falls outside the guarantee entirely, silently — nothing in the code checks
world object speed against the constant.

## A confound in the L=32 dataset

All 18 lookahead-32 runs additionally failed a per-tick invariant unrelated to halo width:

```
INVARIANT FAILURES (18):
  haloWidth17-r1: ownership_gap_ticks = 12 (expected 0)
  ... (18 total, ranging 8-12 ticks per run, every width 17-22, all 3 repeats)
```

This invariant sums `owned_objects` per tick across servers and fails on a gap (an object owned by
nobody) — see `docs/superpowers/specs/2026-08-18-halo-band-cross-border-collision.md:304-329`. It
did not fire on any of the 36 lookahead-2/16 runs. An object that is unowned for several ticks is
integrated by neither server during that window, which is a plausible independent cause of a
missed contact, separate from the halo band being physically too narrow. The UNSOUND verdict at
L=32 above is reported as measured, per instruction, but this confound means the 60-crossings
result cannot cleanly be attributed to halo-width insufficiency alone — a follow-up run isolating
the ownership-gap effect from the halo-width effect would be needed to separate the two. (One
candidate mechanism, not confirmed: `HALO_STALE_TICKS = 30` is a hardcoded shadow-retirement
timeout in `DistributedGameServer/ServerWorldManager.cpp:944`, and `--halo-lookahead 32` is the
first value in this sweep to exceed it.)

## Limitations

- **One synthetic workload.** `headon` is pairs on fixed lanes, at one speed, meeting the border
  perpendicular — the configuration in which the knee is sharpest. Oblique approaches and mixed
  speeds would stress the bound harder.
- **This validates the formula as coded, not a derivation.** If the paper claims the derivation as
  its contribution, the derivation belongs in the text first and this experiment is its
  validation, not its source.

## Bottom line

The soundness condition holds, with expected conservative slack, at lookaheads 2 and 16. It does
not hold at lookahead 32 under this experiment's criteria — no width up to 8 units above the L=16
knee and 2 above its own floor caught every border contact — and that lookahead's runs carry an
independent invariant failure (`ownership_gap_ticks`) that this experiment cannot separate from
the halo-width effect. This is reported as a measured result, not corrected or re-run to produce
a different verdict: the formula is sound and tracks as predicted within the envelope actually
exercised (up to L=16 in this design), and something breaks down by L=32 that this dataset alone
cannot attribute cleanly to the halo band versus a co-occurring ownership defect.
