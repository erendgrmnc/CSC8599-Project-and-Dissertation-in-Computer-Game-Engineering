# Completing the evidence (2026-08-19)

Date: 2026-08-19
Status: **design — approved, not yet run**
Follows: `2026-08-19-experiment-suite.md` (E1–E4), `2026-08-18-scale-ceiling.md`,
`2026-08-16-phd-paper-roadmap.md`

The experiment suite produced four results. This records what is still missing between those four
and a defensible paper, what of it can be answered on the frozen build, and what has to wait for
code.

The ordering rule is the roadmap's own: **fix validity, freeze, measure, then build.** Every code
change invalidates the runs that preceded it, so everything reachable without touching the servers
is done first.

---

## 0. The gap that matters most

The roadmap (§2) demotes predictive handoff — it is dead reckoning, and Aura Projection's
`R_a = R_o + V_t·T_T` already contains a velocity lookahead — and puts the paper's weight instead on
a joint *(lookahead horizon, halo width)* soundness condition: under bounded acceleration, no
cross-boundary contact is missed within a stated envelope.

That condition **is implemented**. `ServerWorldManager::MinimumSafeHaloWidth()` computes

```
w_min = HALO_ASSUMED_MAX_SPEED · L · dt + 2 · HALO_ASSUMED_MAX_RADIUS
```

with `L = --halo-lookahead` in ticks and `dt` the substep. `SetHaloWidth` warns below it. At the
120 Hz substep rate, with the constants as coded (60, 2), this is a line:

```
w_min(L) = 0.5·L + 4
```

**It has never been tested.** E2 ran a single point — `w = 8` at the default `L = 4`, floor 6.0 —
comfortably above the floor. That shows the halo works. It says nothing about why that width, and it
is the only evidence bearing on the claim the paper is built around.

Testing it is the first experiment below, and it needs no code at all.

---

## E5 — The soundness condition

**Claim.** The width required to catch every border contact is predicted by `w_min(L)`, and the
prediction is *sound* — never optimistic — across lookaheads.

### Design

`headon`, 2 servers, 100 objects, 1,800 paced ticks, `--fixed-step`, `--halo-reliable`, 3 repeats.

The sweep is centred on each lookahead's own predicted floor rather than run over one shared range:

| `--halo-lookahead` | predicted floor | swept `--halo-width` |
|---|---|---|
| 2 | **5.0** | 2, 3, 4, 5, 6, 7 |
| 16 | **12.0** | 9, 10, 11, 12, 13, 14 |
| 32 | **20.0** | 17, 18, 19, 20, 21, 22 |

54 runs. Three invocations of `run-experiments.ps1 -Sweep haloWidth`, one per lookahead, with
`-HaloLookahead` held fixed. **No harness or server change is required** — `haloWidth` is already a
sweep axis and `-HaloLookahead` is already forwarded.

> An earlier draft swept a single shared range `{2,4,6,8,10,12,16}` at `L ∈ {2,8,16}`. That resolves
> each knee only to ±2 units while the `L=2` and `L=8` floors are 3 units apart, so the central
> claim would have rested on separating two knees barely one bin apart. Spreading the lookaheads and
> putting 1-unit resolution at each floor gives knees 7 and 8 units apart, located to 1 unit, for
> *fewer* runs.

### Metric

Border crossings, read from `hoSent`. On `headon` every object is launched at a partner across
`x = 0`: a pair that collides bounces and never crosses, a pair whose contact was missed passes
through and is handed off. So crossings count missed border contacts, and the scale is fixed and
interpretable — 100 is total failure, 0 is every contact caught.

This is a **proxy**, deliberately. The direct metric is "contacts a single-server run recorded that
the two-server run did not", which needs a matched baseline run per sweep point and roughly doubles
the cost. On `headon` the proxy and the direct metric coincide; on any other workload they would not,
and the paper must say so rather than generalise the metric.

### What counts as success, and what counts as a result

The prediction has two parts, and they are separable:

1. **Soundness.** The empirical knee sits *at or below* the predicted floor at every lookahead. The
   formula uses assumed maxima (60 units/s) where `headon` actually moves at 30, so the bound is
   deliberately conservative and must never be optimistic. A knee *above* the floor falsifies it.
2. **Tracking.** The knee moves right as `L` grows, by roughly the predicted amount. One knee in the
   right place is a coincidence; three knees that move as predicted is a validated condition.

A failure of (2) at `L = 32` while (1) holds is **also a publishable result**, and arguably a more
interesting one: it would say extrapolation error grows faster than the linear bound allows, which
is exactly the "halo lag is permanent, so keep the lookahead small" argument the halo spec asserts
without measuring.

### The envelope, stated rather than hidden

`HALO_ASSUMED_MAX_SPEED = 60` is a hardcoded global constant, not measured from the world. An object
faster than 60 breaks the guarantee, silently. That is the same posture Aura Projection takes — a
conservative global speed tolerance with the envelope declared (*"if velocities, latencies, or
frame-time are above these tolerances, then stability is no longer guaranteed"*) — and it should be
declared the same way, not presented as unconditional.

Two honest limitations to carry into the write-up:

- **One synthetic workload.** `headon` is pairs on fixed lanes, at one speed, meeting the border
  perpendicular. That is the configuration in which the knee is sharpest. Oblique approaches and
  mixed speeds would stress the bound harder.
- **This validates the formula as coded, not a derivation.** If the paper claims the derivation as
  its contribution, the derivation belongs in the text first and this experiment becomes its
  validation, not its source.

---

## E6 — Density, not count

**Claim.** Per-server cost is governed by objects per broadphase cell, not by object count.

The grid broadphase is linear in objects (measured exponent 1.07), but its constant depends on how
many objects share a cell. A million objects spread thinly is cheap; a hundred thousand in one heap
is not — and a real game world varies density, not count. Named as open in scale-ceiling §4.3.

Fixed world bounds, sweep `-Objects`. `uniform`, 1 server, paced. Report `physicsMs` percentiles
against objects-per-cell rather than against objects. No code.

---

## E7 — Capacity with the halo on

**Claim.** The per-server object budget, measured with cross-border collision enabled.

Every capacity figure quoted so far — ~6,000 objects/server serial, ~10,000 with workers — was
measured with `--halo-width 0`, which is the configuration the paper does *not* advocate. Named as
open in scale-ceiling §4.2.

Sweep `-Objects` on 2 servers at `-HaloWidth 8`, find where p95 tick cost crosses the 8.33 ms budget
at 120 Hz. Compare against the halo-off curve at the same points. No code.

---

## E8 — Bytes, for the Dyconits composition claim

**Claim.** The halo's server↔server cost is smaller than interest management's server→client saving,
so the halo is paid for out of the saving.

The roadmap (§2) calls this the strong composition argument against Dyconits (Donkervliet et al.,
ICDCS 2021), and it is currently qualitative because the harness counts *snapshots*, not bytes.

The wire format is fixed-size POD, so bytes are exactly derivable from the counters already emitted:
`snap_sent × sizeof(snapshot packet)` and `haloObjSent × sizeof(HaloObjectState)`. One run with the
halo and interest management both enabled turns the claim into a number.

**Two methodological requirements**, or the number is wrong in a favourable direction:

- Take the packet sizes from `sizeof` at runtime, not by hand-adding struct members. Padding and
  alignment are not obvious from the declaration.
- **Include per-packet transport overhead.** A `DeltaPacket` is a couple of dozen bytes of payload;
  ENet's header plus UDP/IP adds roughly 36 more. Costing snapshots at payload size alone understates
  the traffic by more than a factor of two, and it understates it *most* for the small packets
  interest management removes — i.e. exactly in the direction that flatters the result. Halo batches
  carry up to 20 entries per packet and are barely affected, so ignoring overhead would compare the
  two on different terms.

No code.

---

## Doc integrity

Two defects `CLAUDE.md` still warns about have been fixed and the warnings never retracted:

| Warning | Reality |
|---|---|
| "Deltas never apply after the first full snapshot" — `mServerSideLastFullID` never written | It **is** written, `DistributedGameServerManager.cpp:293`, from the minimum acked state. The delta path works. |
| scale-ceiling §3.3: `mLastKnownOwner` never prunes | `PruneForwardingTable()` exists and is called each tick from `ServerWorldManager.cpp:1088`. |

Debug-era performance figures remain scattered across four specs under a single general "do not
quote these" note in scale-ceiling §0, rather than being marked where a reader meets them.

For an artifact repository a reviewer will actually read, stale self-criticism reads worse than the
defect would have. Three pieces of work:

1. Retract both warnings above, at the point they appear.
2. Mark Debug-era figures inline, at each figure, not in one distant preamble.
3. Write **one consolidated evaluation document** covering E1–E8 as a single argument. The specs are
   a running log of discoveries in the order they were found, which is the right form for a working
   record and the wrong form for evidence. They stay; the consolidation is additional.

---

## Deliberately deferred to the build phase

Each of these changes the simulation or the harness, so each invalidates the runs above and must
follow them.

| | Why it waits |
|---|---|
| **Multi-machine** | Needs hardware. Parked by decision. The only route from a locality claim (I6) to a speedup claim; nothing in the code has to change, only the run configuration. |
| **AP-comparable injection workload** | Aura Projection's benchmark is 160 objects/s injected for 60 s. Runtime spawn exists, so this is a new `--workload injection`. The roadmap calls it the highest evaluation-value-per-hour item, because without it there is no comparable graph against the direct ancestor. |
| **Latency injection** | The soundness envelope is stated in terms of latency and every run is localhost at ~0 ms. E5 measures the halo-lag term with `L`; it does not measure the network term. |
| **Handoff ack (B1)** | `HandleTransitionHandshakeReceived` is an empty body. Narrower than previously recorded: transfers go over a **reliable** ENet channel and the refused-send path is handled, so ordinary loss is covered by retransmission. The real exposure is peer death or connection teardown mid-handoff. |
| **Ownership gap under rebalancing (B2)** | E4 found ~2,538 gap ticks on every dynamic run, nothing lost. Atomic transfer holds only while both servers keep pace, which a migration by definition violates. Either fixed or stated as a conditional guarantee. |
| **Contact-weighted load profile (B3)** | The profile buckets object counts, so the policy equalises objects and leaves a residual contact imbalance (14.2 M against 8.1 M at 2,007/1,993 objects). |
| **`CalculateIncomingObjectOffsetPosition`** | Still never called. Wiring it changes measured handoff behaviour, so it belongs with a re-measurement, not as a drive-by. |

---

## Order of work

1. E5 — 54 runs, three invocations. The headline.
2. E6, E7, E8 — the remaining gaps in the scale story.
3. Doc integrity, including the consolidated evaluation document.
4. Re-open the build phase, in the table's order.
