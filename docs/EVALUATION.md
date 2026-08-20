# Evaluation

The specs under `docs/superpowers/specs/` and the results under `docs/superpowers/results/` are a
running log of discoveries in the order they were found — the right form for a working record, the
wrong form for evidence. A reader currently has to reconcile eleven documents, several of which
correct each other (round 1 vs round 2 of E5, the retracted E8 bandwidth figures, the corrected E7
capacity numbers). This document presents E1-E8 as one argument: what is claimed, how it was
measured, what the numbers say, and what they do not show. The specs and results documents stay as
the primary record; every figure below is transcribed from them, not re-derived, and traceable to a
run directory under `runs/` or to a named results document.

---

## 1. What is claimed

- **Locality (I6).** Per-server owned state scales with region occupancy, not with total world size
  — the quantity that decides whether the design lets a world grow without a per-server cost growing
  with it.
- **Cross-border correctness.** Objects either side of a region border collide only when the halo
  band mechanism is enabled; without it they silently pass through each other at the border.
- **The soundness condition.** The halo width formula `w_min = v_max * L * dt + 2*r_max` never
  under-predicts the width actually required to catch every border contact, at any tested lookahead
  `L`.
- **The client-facing cost bound.** A client's snapshot traffic is a property of its interest radius
  (what it can see), not of total world size.
- **Dynamic balancing.** Moving region borders at runtime reduces the cost borne by the busiest
  server, relative to a static partition on an adversarial workload.

---

## 2. How it is measured

- **Paced vs realtime, chosen per claim.** `--run-ticks` with `--fixed-step` pins every server to one
  shared clock, so end state and conservation reproduce exactly — the right mode for correctness and
  per-tick cost claims (E1, E2, E4-E7). `--run-seconds` bounds by wall clock and is the right mode for
  a claim that is inherently a *rate* — snapshot volume per second — where a paced run would report
  whatever the pacing happened to allow (E3, E8).
- **Repeats and the median.** Per-server object counts and handoff event counts vary by ±1 at the same
  seed, so every point in every experiment below is 3 repeats; reported figures are the **median**
  across repeats, per `analyse.py`.
- **Percentiles, not means.** Per-tick physics cost is bimodal (most ticks pay only the empty-iteration
  cost, a minority pay a real substep), so a mean blends two different regimes into a number that
  describes neither. p50/p95/p99 are reported instead; p95 is treated as the headline figure where one
  number is needed, because it is the one steady enough at n=3 to trust.
- **Never pooled across servers.** Servers carry different loads and tick at different rates, so
  summing or averaging ticks across servers would weight whichever server ticked more. Per-server
  figures are reported, and the **busiest server** is called out separately — the run is no faster
  than its slowest participant.

---

## 3. E1-E8

### E1 — Locality (I6)

**Claim.** Per-server state scales with region occupancy, not world size.

**Configuration.** `uniform`, 1,800 paced ticks, 3 repeats, world and object count grown together with
server count (`runs/exp-locality`).

| servers | world objects | per-server owned (median) | max / min |
|---|---|---|---|
| 1 | 400 | 400 | 400 / 400 |
| 2 | 800 | 400 | 405 / 395 |
| 4 | 1,600 | 400 | 404 / 397 |

**Verdict.** The world grows 4x and per-server owned state does not move. All invariants hold on all 9
runs.

### E2 — Cross-border collision

**Claim.** Objects either side of a region border collide only when the halo band is on.

**Configuration.** `headon` — pairs launched at each other across `x = 0`, so every recorded collision
is a border collision. 100 objects, 2 servers, 1,800 paced ticks, 3 repeats (`runs/exp-halo`).

| halo width | border crossings, per repeat | contacts (median) |
|---|---|---|
| 0 | 100, 100, 100 | 76,600 |
| 8 | 0, 0, 0 | 82,390 |

**Verdict.** Without the halo every pair passes through its partner. With it, nothing crosses —
identical across all three repeats in both configurations. The higher contact count with the halo on
(82,390 vs 76,600) is expected: each border contact is resolved on both servers by design.

### E3 — Interest management

**Claim.** A client's snapshot cost is a property of its view, not of the world.

**Configuration.** 4,000 objects, 2 servers, 20 s realtime, 3 repeats (`runs/exp-interest`).

| interest radius | object-snapshots sent (median)¹ | reduction |
|---|---|---|
| 0 (everything) | 1,317,106 | — |
| 100 | 589,128 | 55.3% |
| 50 | 374,819 | 71.5% |
| 25 | 277,500 | 78.9% |

¹ See caveat below — contains an unverified drain-phase artefact; absolute counts not yet clean to quote.

**Verdict.** Monotone in radius, as it must be. **Caveat, not yet checked:** these runs contain the
drain-phase artefact identified under E8 (an unthrottled ~5-second tail that dominates several
per-tick counters). The *ratios* here may survive it — a drain-phase contribution roughly constant
across radii would cancel out of a percentage reduction the way it cannot cancel out of an absolute
bytes/second figure — but that has not been verified, and the absolute counts should not be quoted as
clean until it is.

Note the halo publish gate (§4.1) does **not** fix this one: snapshot broadcast was already rate-gated,
so the tail here is 5 s of genuine 60 Hz snapshots on a 20 s run, not a spin-rate flood. What clears it
is backlog item 5 — `--drain-seconds` is now forwarded, so E3 can be re-run with the drain set to 0 and
the absolute counts made quotable.

### E4 — Dynamic load balancing

**Claim.** Moving borders at runtime reduces the cost borne by the busiest server.

**Configuration.** `cluster` — objects packed into one part of the world and staying there, the only
workload that can judge a balancer. 4,000 objects, 2 servers, 7,200 paced ticks, 3 repeats
(`runs/exp-balance`).

| rebalance interval | busiest p95 ms | objects | busiest : lightest |
|---|---|---|---|
| 0 (static) | 11.83 | 4,000 / 0 | 8,452 : 1 |
| 400 ticks | 10.50 | 2,007 / 1,993 | 1.71 : 1 |

**Verdict.** Object balance is essentially perfect and conservation is exact on every run. The
busiest-server timing improvement is **11%, not the 30% a single run suggested** — the single-run
figure measured wall clock over the whole run, dominated by the interval before the partition
converged; the p95 per-tick figure here is the steadier, honest measure, and exactly what repeats were
supposed to catch. Every dynamic run showed ownership gaps (2,574 / 1,891 / 2,538 ticks, median 2,538)
that track `hoLate` almost exactly, with conservation exact throughout — nothing is lost, but the
atomic-ownership guarantee is conditional (§6).

### E5 — Halo soundness

See §4 below — given its own section as the headline result.

### E6 — Density

**Claim.** Per-tick physics cost stays close to linear as object density rises on a fixed-size world
(the world is *not* grown with the object count here, unlike E1 — the point is to see how a fixed
region degrades under clustering).

**Configuration.** 1 server, `uniform`, fixed 300x300 world, 1,800 paced ticks, 3 repeats, objects
swept 500 to 8,000 (`runs/exp-density`; full detail in
`docs/superpowers/results/2026-08-19-E6-density.md`).

| objects | p50 ms | p95 ms | p99 ms |
|---|---|---|---|
| 500 | 0.659 | 1.096 | 1.413 |
| 1,000 | 1.131 | 1.574 | 2.258 |
| 2,000 | 2.191 | 3.055 | 3.975 |
| 4,000 | 4.583 | 6.851 | 9.179 |
| 8,000 | 10.524 | 14.808 | 17.385 |

**Verdict.** Fitted exponent 1.09 over the 1,000-8,000 range (R^2 = 0.998) — mildly super-linear, not
quadratic. No cliff or sharp knee anywhere in the tested range (up to 0.356 objects/cell on a 22,500-
cell grid, cell size 2.0 from `maxExtent = 0.5`). A load balancer using a linear cost model will be
consistently a little optimistic at higher density, not badly wrong.

### E7 — Per-server capacity

**Claim.** The real per-server object budget, with cross-border collision enabled, is lower than the
previously published halo-off figures.

**Configuration.** 2 servers, `uniform`, `--fixed-step`, 1,800 paced ticks, 3 repeats, objects swept
1,000/2,000/4,000/8,000, both `--halo-width 0` and `--halo-width 8` (`runs/exp-capacity-halo`,
`runs/exp-capacity-nohalo`; full detail in `docs/superpowers/results/2026-08-19-E7-capacity.md`). The
planned 12,000-object point was dropped for time; the halo-off crossing below is therefore
extrapolated, not bracketed.

| configuration | crossing (total, 2 servers) | crossing (per server) | how obtained |
|---|---|---|---|
| `--halo-width 0` (prior published figures) | ~8,893 | ~4,447 | extrapolated past the swept range |
| `--halo-width 8` (this work's advocated configuration) | ~6,767 | ~3,384 | interpolated, bracketed by measured data |

**Verdict.** The halo costs 25.8% of the 8.33 ms (120 Hz) tick budget at its own crossing point. Below
that budget, ownership gaps are bounded and conservation is exact at every measured point; at the
8,000-object point (over budget), gap ticks reach 1,795-1,798 of 1,800 and `conservation_delta` runs
-4 to -8 — objects are actually lost. **The headline is that the per-server budget is a correctness
limit, not only a performance one**: ~3,384 objects/server is where the simulation stops being
correct, not merely where it gets slow.

### E8 — Bandwidth

**Claim under test.** The halo's server-to-server cost is paid for out of interest management's
server-to-client saving.

**Status: UNTESTED, not refuted.** Two measurement-harness defects contaminate the byte counters this
claim would be built from (`runs/exp-bytes`, `runs/exp-bytes-paced`; full detail in
`docs/superpowers/results/2026-08-19-E8-bandwidth.md`):

- `PublishHaloBand()` has no rate gate, unlike the 60 Hz-gated snapshot broadcast.
- `--drain-seconds` is parsed by the game server but never forwarded by the midware, so the
  unthrottled ~5-second drain phase that follows every timed run cannot be disabled through any
  supported tooling path. A diagnostic sweep (`runs/exp-halo-diag`) showed combined `haloObjSent`
  varying under 3% across a 20x range of main-loop duration — proof the drain phase, not the timed
  loop, dominates the counter.

**Two previously reported figures (25.7 MB/s realtime, 33.9 MB/s paced) are retracted** — they measure
the drain-phase artefact, not the halo's real cost. A geometric estimate (~1.6 MB/s, explicitly not a
measurement) suggests the claim would plausibly hold if cleanly measured, but this is not evidence and
must not be cited as such. E8 is blocked on the frozen build (`PhysicsServerMidware/` is frozen for
this evidence pass to protect E5's 120 runs, which depend on the current drain-phase behaviour) and is
left untested rather than force-completed. **E8 must be re-run once the rate gate (build-phase item 4)
and the `--drain-seconds` forwarding (item 5) land** — see §6 of the backlog below.

---

## 4. The soundness condition (E5)

This is the project's headline correctness result, so it gets its own section rather than a row in the
table above. Full detail: `docs/superpowers/results/2026-08-19-E5-soundness.md`.

**Claim.** `MinimumSafeHaloWidth()` predicts a halo band width — `w_min = v_max * L * dt + 2*r_max`,
the "conservative floor" — that never under-predicts the width actually required to catch every
cross-border contact, at a given handoff lookahead `L`.

**Configuration.** `headon`, 2 servers, 100 objects, 1,800 paced ticks, `--halo-reliable`, 3 repeats
per point. 120 runs total across two rounds (`runs/exp-haloL2`, `exp-haloL16`, `exp-haloL32` — round 1;
`runs/exp-haloL2b`, `exp-haloL8b`, `exp-haloL16b`, `exp-haloL24b` — round 2).

**Round 1's design could not bracket a knee.** Six points centred on each lookahead's own conservative
floor returned all-zero or all-failing results at every point sampled, because `headon` moves objects
at 30 units/s against the formula's assumed maximum of 60 — the true knee sits well below the window
searched. This is disclosed rather than hidden: round 1 establishes sufficiency in the weak sense at
L=2 and L=16 (nothing sampled ever failed) but not tracking, and it surfaced a separate, real finding
at L=32 (below).

**Round 2 swept upward from 0** (halo disabled) at each of four lookaheads, targeting the
actual-speed-implied region rather than the conservative floor:

| lookahead | conservative floor | empirical knee | slack | verdict |
|---|---|---|---|---|
| 2 | 5.0 | <=1 | >=4 | sound |
| 8 | 8.0 | 3 | 5 | sound |
| 16 | 12.0 | 4 | 8 | sound |
| 24 | 16.0 | 6 | 10 | sound |

**Soundness holds at every lookahead tested** (1<=5, 3<=8, 4<=12, 6<=16). **Tracking holds**: the knee
increases monotonically with lookahead, and three of the four transitions (L=8, 16, 24) rest on a
directly observed failing-width-immediately-below-a-passing-width transition, not an inferred one; the
L=2 point is a bound (<=1), not a located measurement, because the sweep's lower limit is 0 (a
mechanism on/off switch, not a real width). **Slack widens with lookahead** (4 to 10) — the bound gets
more conservative, not less, as lookahead grows. **The functional form is explicitly undetermined**: a
mid-experiment prediction (`0.25*L + 1`, giving 5 at L=16 and 7 at L=24) was stated in advance and
refuted twice (measured knees were 4 and 6); the refutation is reported as a refutation, not re-fit.

**The declared envelope: the condition has an upper bound on lookahead, between 24 and 32.** At every
lookahead up to 24, widening the band drives missed contacts to *exactly zero*. At lookahead 32 it does
not, and no width recovers it: crossings sit at 60 (of 100 objects) at widths 17, 18, 19, 20, 21 and 22
alike — flat, and specifically unchanged at and above the conservative floor of 20.

| lookahead | crossings vs width | knee |
|---|---|---|
| 8 | 50 -> 22 -> **0** from width 3 | 3 |
| 16 | 50 -> 50 -> **0** from width 4 | 4 |
| 24 | 50 -> 50 -> **0** from width 6 | 6 |
| 32 | **60, 60, 60, 60, 60, 60** at widths 17-22 | none |

This corrects an earlier explanation recorded here, which attributed the L=32 failure to
`HALO_STALE_TICKS = 30` (`ServerWorldManager.cpp:944`) retiring shadows before they could be applied.
Re-analysis of round 1's own per-tick CSVs refutes that: on the L=32 runs `haloLate = 0` (every update
landed in its slot) and `halo_objects` averages 18.2 with shadows present on 98.2% of ticks. Nothing
was being retired, and `HALO_STALE_TICKS` is not involved.

With delivery, retirement and band width all eliminated, the remaining term in a shadow's placement is
the extrapolation itself: at lookahead 32 a shadow is dead-reckoned 32 ticks — 0.267 s at the 120 Hz
substep — ahead of its sample, which at 30 units/s puts it 8 units, two body diameters, from where the
object actually is. A contact resolved against a shadow that far out of position is resolved against
the wrong geometry, and widening the band cannot fix a placement error. **This is an inference by
elimination, not a measurement**: confirming it means disabling extrapolation and re-running, which is
a simulation-affecting change and is recorded for the build phase rather than claimed here.

The practical statement is therefore weaker than "the formula holds" and stronger than "L=32 was never
tested": the formula is sound over the measured range L in [2, 24], and there exists an upper bound on
lookahead, somewhere in (24, 32], past which no band width satisfies the condition — which is the
"halo lag is permanent, so keep the lookahead small" argument the halo spec asserted without evidence,
now with evidence.

### 4.1 Re-validation after the halo publish gate

E2 and E5 were measured on a build that published the halo band once per *loop iteration* rather than
once per tick. Since the 5 s drain phase runs the loop without stepping the world, any run that ended
with objects still inside the band republished that band continuously with the tick counter frozen.
The effect is large where it applies: on the L=32 width-20 run `haloSent` reached 178,693 and 173,943
on the two servers against 2,101 each for the same configuration gated, an 82x reduction in halo
objects sent — and note the two figures differ from one another, because each server was spinning at
whatever rate it could, which is server-to-server non-determinism inside a run that was supposed to be
reproducible.

It reached only the sweeps that end with objects in-band: **L=24 at widths >= 6, and all of L=32**.
Every other halo sweep ran clean (`haloSent` 15-400), E2 included, which is consistent with E2's runs
ending with zero objects in band.

Both affected results were re-measured on the gated build:

| | before | after |
|---|---|---|
| E2 (`headon`, widths 0 and 8, 3 repeats) | crossings 100/0, contacts 76,600/82,390 | **identical, all 6 runs** |
| E5 L=24 knee (widths 4-7, 2 repeats) | knee at width 6 | **knee at width 6** |

Neither moved, because both are read from *crossings*, which the flood did not affect. What it did
affect is contact totals and contact symmetry: on the L=32 width-20 run the two servers resolved
55,695 and 55,830 contacts — a violation of invariant I8 (symmetric contact) — where the gated run has
both resolving exactly 55,980. No figure quoted in this document is drawn from a flooded run, but the
distinction matters for anything read from those CSVs later: **their crossing counts are trustworthy
and their contact counts are not.**

---

## 5. What this evidence cannot show

- **Single-machine throughout.** Every run puts every server, the manager, the midware and the client
  on one 6-core machine. Wall-clock comparisons across server counts (E1, E4, E6, E7) therefore measure
  contention on shared hardware, not distribution across separate machines. E1 is safe from this — it
  measures state, not time — and E2 is safe — it measures events. E4's and E7's timing figures compare
  two partitions of the same total work on the same hardware, which is a fair comparison, but not a
  distributed speedup claim. Turning any of it into one needs one server per machine; nothing in the
  code has to change for that, only the run configuration.
- **No latency injection.** Every measurement runs on a local network with effectively zero link
  latency. Nothing here says how any of these claims hold up once cross-server or client-server
  messages carry real network delay.
- **No AP-comparable injection workload.** These experiments were not designed against an established
  AP-system benchmark or fault-injection methodology; comparisons to other distributed physics or
  Dyconits-style systems would need a shared workload definition that does not exist yet.
- **`headon` is one synthetic workload.** E2 and E5's soundness sweep both rely on `headon` — pairs on
  fixed lanes, one speed, meeting the border perpendicular — which is the configuration in which the
  halo's knee is sharpest and easiest to locate. Oblique approaches, mixed speeds, and denser traffic
  would stress the bound harder and are untested.
- **E8 is untested**, not refuted (§3 above) — the bandwidth composition claim remains genuinely open.

---

## 6. Known conditional guarantees

**Ownership atomicity holds only while both servers keep pace.** The B6 mechanism makes ownership
transfer atomic by having the sender release at the same `senderTick + lookahead` tick the receiver
installs on — but only when both servers reach that tick at comparable real times. A rebalancing
migration violates this by definition, because the partition is, by construction, still imbalanced
during the move: E4 measured ~2,538 median ownership-gap ticks per dynamic run, tracking `hoLate`
almost exactly, with conservation exact and nothing lost. E7 found the same mechanism under sustained
overload rather than a transient migration, and there the consequence is worse: past the ~3,384
objects/server correctness budget, gaps become near-permanent (1,795-1,798 of 1,800 ticks) and objects
are actually lost (`conservation_delta` -4 to -8 at 8,000 objects). The guarantee is real but
conditional, not unconditional, and both experiments independently expose the same underlying failure
mode.

**The load balancer equalises objects, not contacts.** E4's dynamic case ends with near-perfect object
balance (2,007 / 1,993, the 3-repeat median from `runs/exp-balance`) but residual contact imbalance
(14.2M vs 8.1M) — object count is an imperfect proxy for the physics work a server actually performs,
since a cluster of overlapping objects costs far more per object than a sparse one. The two figures are
from different runs: the contact counts are a single run recorded at
`docs/superpowers/specs/2026-08-18-dynamic-repartitioning.md:258` (object split 2,023/1,977 there, not
2,007/1,993), whose table carries a "Debug build — not quotable (duration rows only)" annotation. That
annotation does not disqualify the contact figures — contact *counts*, unlike durations, are
build-independent — but the object split quoted above is E4's own median, not that run's.

---

## 7. The build-phase backlog

Every item below was found by measurement during this evaluation pass, not by code review.

The server directories were frozen while E1-E8 were measured, specifically to keep E5's 120 runs valid
against a fixed binary. That freeze is over. Items 3, 4, 5, 8 and 9 were fixed afterwards as **Batch
A**, chosen because none of them could alter a simulation result — the one that turned out to (item 4)
was caught by re-measuring E2 and E5's L=24 knee against the new binary before anything was claimed
(§4.1). Items 1, 2, 6 and 7 remain, are all simulation-affecting, and belong to a single **Batch B**
followed by one full re-measurement.

1. **Handoff ack is stubbed.** `ServerWorldManager::HandleTransitionHandshakeReceived` has an empty
   body; `NetworkObject::OnTransitionHandshakeReceived` is never called. A dropped transfer packet
   still loses the object with no retry path.
2. **Ownership atomicity fails above the tick budget.** E7 (§3 above), and E4's rebalancing case (§6
   above). The guarantee is conditional on both servers keeping pace, not unconditional.
3. ~~**No guard on halo lookahead.**~~ **Withdrawn — this was a mis-diagnosis, not a defect.**
   Re-analysis of E5 round 1's own CSVs shows `haloLate = 0` and shadows present on 98.2% of ticks at
   lookahead 32, so nothing was retiring and `HALO_STALE_TICKS` was never involved. There is no
   "silently disabled" threshold to guard. The real behaviour at lookahead 32 is an upper bound on the
   soundness condition, now recorded as a result in §4 rather than as a bug. What *does* remain open is
   confirming the mechanism: §4 attributes it to extrapolation error by elimination, and proving that
   means disabling extrapolation and re-running.
4. ~~**`PublishHaloBand()` has no rate gate.**~~ **Fixed.** Now published once per tick rather than
   once per loop iteration. It was worse than a bandwidth problem: because the drain phase runs the
   loop without stepping the world, affected runs republished the whole band with the tick counter
   frozen, at rates that differed between the two servers (178,693 vs 173,943 sends on one L=32 run) —
   non-determinism inside a supposedly reproducible run — and the resulting traffic broke invariant I8,
   with the two servers resolving 55,695 and 55,830 contacts where the gated build has both at exactly
   55,980. See §4.1, including the re-validation of E2 and E5's L=24 knee.
5. ~~**`--drain-seconds` is never forwarded by the midware.**~~ **Fixed.** Forwarded like the dozen
   other game-server flags. Note item 4 was the actual E8 blocker; this one now only gives control over
   the drain, rather than being needed to escape it.
6. **Load profile buckets objects, not contacts.** E4's residual contact imbalance (14.2M vs 8.1M, §6
   above, sourced from a different run than the object split it is quoted alongside — see §6) despite
   near-perfect object balance.
7. **`CalculateIncomingObjectOffsetPosition` is never called.** `ServerWorldManager.cpp:440`. Incoming
   handoffs get no positional nudge into the receiving region.
8. ~~**Stale comments in frozen source.**~~ **Fixed.** `NetworkObject.h` now states 60 bytes per halo
   entry (as `PacketSizeTests` measures) and the snapshot gate comment states 60 Hz.
9. ~~**`run-experiments.ps1`'s `-OutDir` is not anchored to the repo root.**~~ **Fixed.** Both scripts
   now share `tools/RunPaths.ps1`, which also rejects the drive-relative case (`C:runs`) that
   `Path.IsPathRooted` reports as absolute. Shared rather than copied precisely because this bug
   existed only because the earlier fix was applied to one script and not the other.

**Items 3, 4, 5, 8 and 9 are now closed** (Batch A). Items 1, 2, 6 and 7 remain, and all four are
simulation-affecting, so they belong to a single batch followed by one full re-measurement.

**E8 is now unblocked and has not yet been re-run.** The halo publish gate was its prerequisite and is
in; the bandwidth composition claim stays untested until that run happens.
