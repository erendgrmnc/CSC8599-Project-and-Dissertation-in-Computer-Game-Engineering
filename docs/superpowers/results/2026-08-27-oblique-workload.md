# The oblique workload, and whether the lag ceiling is a `headon` artefact

**Date:** 2026-08-27
**Closes:** Phase C spec §4.7 (the optional companion), roadmap §6.2 item 3

## Why

The halo soundness condition's **total-lag ceiling** — above roughly 200 ms of
sample-to-apply lag, no band width catches every border contact — was measured on exactly
one workload. `headon` is fixed lanes, one speed, and a strictly perpendicular approach:
every pair meets at `x = 0` at the same instant, which is the most collision-dense
configuration the border can have. Roadmap §6.2 item 3 records the consequence: the ceiling
is "plausibly a property of the workload rather than of the design", and `headon` alone
cannot tell you which.

## What `oblique` changes, and what it deliberately does not

Two variables, everything else held identical (`ObliqueWorkload.h`):

- **Approach angle**, linear in `z` across the world extent, 0 to 45°. Pairs still converge
  on `x = 0`, but obliquely, so dead-reckoning placement error acquires a **tangential**
  component. A perpendicular approach cannot produce one — the error is collinear with the
  closing direction, which is the easiest case for a band measured perpendicular to the
  border.
- **Speed**, derived rather than configured: `|v| = perpSpeed / cos(theta)`, 30 to 42.4 u/s.
  `HALO_ASSUMED_MAX_SPEED` therefore becomes a genuine upper bound rather than the exact
  speed of every object — the premise the published bound is stated under, never previously
  tested.

The **perpendicular component is held at `headon`'s own 30 u/s**, so time-to-contact is
identical on every lane and identical to `headon`. `headon`'s constants are pinned between
"too slow and it lands before the border" and "too fast and it tunnels"; varying the
perpendicular speed would leave that window and measure the integrator instead.

The angle is keyed on **position**, not grid index, for the reason `headon`'s own comment
gives about its direction test: the index-to-cell mapping lives in `SetupWorld` and reading
it back elsewhere is a second place to keep in step. It varies **slowly** — adjacent lanes
are 6 units apart in a 300-unit world, so under a degree apart — because randomly assorted
angles would let neighbouring pairs drift into each other and contaminate the contact count
with collisions that are not the head-on one.

## Result 1: at zero latency the two workloads are indistinguishable

Widths 2-8, `--halo-lookahead 4`, 3 repeats, with a `headon` control on the same binary:

| workload | empirical knee | crossings at widths 2-8 |
|---|---|---|
| `oblique` | 2 | 0 at every width |
| `headon` | 2 | 0 at every width |

Identical. The bound predicts a floor of 6 and the real knee is at or below 2, so it is
**conservative by roughly 3x** — which is the direction a soundness bound must err in.

**A first pass sampled only widths {0, 8} and `analyse.py` reported "empirical knee 8,
predicted 6, UNSOUND - the bound was optimistic".** That was a sampling artefact and is not
a finding: the smallest width tested was 8, so the knee could not be reported as anything
lower. It is recorded here because it is the same error Phase C §4.8 records against its own
passes 1 and 2, in the opposite direction, and it would have read as a falsification of the
project's headline claim.

## Result 2: the ceiling is real in both, and its severity is not

`--halo-lookahead 32` — the configuration where `headon` is known to fail at every width —
widths 8, 16, 24, 32, 3 repeats, both workloads on the same binary:

| workload | missed contacts (median) | across widths 8-32 |
|---|---|---|
| `headon` | **60** | flat |
| `oblique` | **42** | flat |

**Two separate conclusions, and they point different ways.**

1. **The ceiling generalises.** `oblique` also fails at every width, and fails *flat* — the
   defining signature, that no band width covers a dead-reckoning placement error. The
   qualitative claim in `EVALUATION.md` §4 and roadmap §6.1 is **not** a `headon` artefact.
   This is the answer §6.2 item 3 asked for.
2. **The number 60 is workload-specific.** `oblique` loses 42 where `headon` loses 60 — and
   against their own all-missed baselines (100 and 98 at width 0) that is 60% versus 43%.
   The *severity* of the ceiling depends on the geometry, so 60/100 should be quoted as a
   `headon` figure and not as the ceiling's magnitude in general.

`headon` reproduces its published 60 exactly on this binary, which is what makes the
comparison attributable to the workload rather than to any change since.

## What this does not show

- **One lag point.** `--halo-lookahead 32` at zero latency. The ceiling was also reached via
  `L = 40 / 300 ms`; that second route is not re-run here for `oblique`.
- **Why 42 and not 60.** Both members of an oblique pair share one tangential velocity, so
  their *relative* approach is identical to `headon`'s — the difference must come from the
  absolute velocities, which is where dead reckoning does its extrapolating. Attributing it
  properly needs the run with extrapolation disabled that Phase C §4.8 already lists as
  outstanding, and it is not attempted here.
- **Two servers, one band configuration, zero jitter.**
