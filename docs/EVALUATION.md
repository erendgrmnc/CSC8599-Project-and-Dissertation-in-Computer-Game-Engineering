# Evaluation

The specs under `docs/superpowers/specs/` and the results under `docs/superpowers/results/` are a
running log of discoveries in the order they were found — the right form for a working record, the
wrong form for evidence. A reader currently has to reconcile eleven documents, several of which
correct each other (round 1 vs round 2 of E5, the retracted E8 bandwidth figures, the corrected E7
capacity numbers). This document presents E1-E8 as one argument: what is claimed, how it was
measured, what the numbers say, and what they do not show. The specs and results documents stay as
the primary record; every figure below is transcribed from them, not re-derived, and named against
the run directory that produced it or a named results document.

> **The run directories themselves are gone** (recorded 2026-08-23; this paragraph previously
> described figures as "traceable to a run directory under `runs/`"). `runs/` is gitignored
> (`.gitignore:142`), so no dataset behind any figure here was ever committed — `exp-locality`,
> `exp-halo`, `exp-interest`, `exp-balance`, `exp-density`, `exp-capacity-halo`,
> `exp-capacity-nohalo`, `exp-bytes-clean`, the seven E5 sweep directories, `exp-fix4-E4`,
> `exp-fix4-E7` and `exp-ap-injection-paced` are all absent from a fresh clone. Every `runs/...`
> reference below names the configuration that produced a figure; **it does not name a directory a
> reader can open.** The figures and the per-run detail in the results documents are the surviving
> record.
>
> The consequence is methodological, not cosmetic: **no question about these numbers can be settled
> by re-analysis.** Anything of the form "do the old runs still say X if we recompute Y" — E3's
> ratios under the drain artefact (§3), E4's loss against `ho_parity_delta` (§6), E8's modelled
> bytes (§3) — requires re-running, not re-reading. Sequenced in
> `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` §1.1, which makes generating a
> fresh baseline the first step of every phase.

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
- **Never pooled across servers** — with one deliberate, disclosed exception. Servers carry different
  loads and tick at different rates, so summing or averaging ticks across servers would weight
  whichever server ticked more. Per-server figures are reported, and the **busiest server** is called
  out separately — the run is no faster than its slowest participant. E8's per-tick normalisation
  tables and cross-session cross-check (§3) knowingly pool across servers anyway, because a
  cross-session or cross-client-count comparison has no other natural way to combine two servers into
  one figure; see §3 for the disclosure, the per-server breakdown, and the robustness check.
- **Every manifest in this phase's runs records `gitDirty: true`**, and `analyse.py` prints
  `(DIRTY - not reproducible)` for each of them. This is benign here — the only differences are the
  CMake-regenerated `.sln`/`.vcxproj.filters` files, not source — but it was previously undisclosed in
  this document; noted here rather than left implicit.

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

**Configuration.** 4,000 objects, 2 servers, `uniform`, 20 s realtime, 3 repeats,
`--drain-seconds 0` (`runs/exp-interest-clean`).

| interest radius | object-snapshots sent (median) | raw reduction | tick-normalised reduction | previously published |
|---|---|---|---|---|
| 0 (everything) | 3,937,527 | — | — | — |
| 25 | 519,836 | 86.8% | **89.7%** | 78.9% |
| 50 | 824,292 | 79.1% | **83.6%** | 71.5% |
| 100 | 2,680,918 | 31.9% | **46.3%** | 55.3% |

**Verdict.** Monotone in radius in both columns, as it must be (25 > 50 > 100), the same direction as
the originally published 78.9 / 71.5 / 55.3%, and the claim is well supported: at radius 25 the
reduction is **86.8% raw / 89.7% tick-normalised — both above the published 78.9%** — meaning a client
watching a 25-unit radius receives roughly one-eighth of the world's snapshot traffic. The
tick-normalised figures **beat** the published percentages at radii 25 and 50 (89.7% vs 78.9%; 83.6%
vs 71.5%); only radius 100 falls short of its published figure (46.3% vs 55.3%), and even there
interest management still removes roughly a third to just under half the traffic (31.9% raw / 46.3%
tick-normalised — cite both, per this section's own rule below, not the normalised figure alone). The
caveats below qualify the precision of the absolute figures and the radius-100 magnitude specifically
— they do not put the qualitative result in question.

`--drain-seconds 0` throughout removes the drain-phase artefact these figures previously carried, but
that does not make the raw column unconditionally "clean to quote": a `--run-seconds` run is unpaced,
so a 20 s window's raw total is a property of the machine at that minute (this session's r=0 run ticked
3,425 times, summed across both servers, in its 20 s window; a different session would tick a
different amount), while snapshots-per-tick is the configuration-invariant quantity and is reported
alongside for that reason. The gap between the two is largest exactly at the heaviest, most
tick-starved point: radius 100 differs by **14.4 percentage points** between raw (31.9%) and
tick-normalised (46.3%), and radius 100 is also the row that diverges most from the published 55.3%.
Report both columns rather than treating either alone as settled.

**Cross-check restored, and it now fails on raw counts.** The published document carried an
independent cross-check: the radius-0 figure implied ~1.36 M object-snapshots, against 1,317,106
recorded independently by E3 — agreement within 3%. The same check on this session's own data: E8's
r=0 (halo **on**, strictly more work, `runs/exp-bytes-1client`) recorded 4,897,037 snapshots over
4,384 summed ticks, against E3's r=0 (halo off) 3,937,527 over 3,425 summed ticks — **24% apart on raw
counts, with E8 (the heavier configuration) higher**, the wrong direction for two independent
20-second measurements of the same object count. Normalised per tick it reconciles: 4,897,037 / 4,384
= **1,117** snapshots/tick (E8), 3,937,527 / 3,425 = **1,150** snapshots/tick (E3) — agreement within
**2.8%**. The two runs simply ticked a different number of times inside their respective windows;
per-tick throughput was consistent, which is what a within-machine cross-check actually tests.

That 2.8% figure **pools ticks across both servers**, which §2 above states this document never does
("summing or averaging ticks across servers would weight whichever server ticked more"). It is
knowingly set aside here, not overlooked: E3's own r=0 run ticked its two servers 2,190 and 1,235
times (1.77:1, unusually asymmetric — every other radius in this experiment ticked close to evenly),
so the pooled figure partly reflects that imbalance. Per server the cross-check is **0.6% on server 0**
(1,108 vs 1,102 snapshots/tick) and **8.0% on server 1** (1,223 vs 1,132) — both still close, so no
conclusion above changes, but the 2.8% headline number is tighter than either individual server, not a
neutral average of them. A robustness check (rescaling server 1 to server 0's tick rate) moves the r=0
normaliser from 1,150 to ~1,164 snapshots/tick, a 1.3% shift — again nothing that moves a verdict, but
recorded because §2's rule is being deliberately, not silently, set aside for this one cross-session
comparison, where per-server figures don't have a natural way to combine.

**This same pooling applies to two more tables in this document, not only to this 2.8% figure.** The
1-client peer-B/tick table above and the 2-client peer-B/tick table below both divide bytes summed
across servers by ticks summed across servers, and it matters more there than it does here: E8's own
radius-0 median repeat ticks its two servers asymmetrically (1,860 / 1,554), and per-server halo B/tick
can differ by up to 26% within a single repeat at equal ticks (radius-0 repeat 3: 7,148 vs 9,013). §2's
"never pooled across servers" rule is set aside for all three of these tables, for the same reason —
there is no other way to combine two servers into one across-session or one 1-vs-2-client comparison —
and that exception should be read as covering the peer-B/tick tables as well as the cross-check above,
not only the figure it happens to be attached to here.

**Not all repeats were captured in one continuous session.** File timestamps show the original
`exp-interest-clean` sweep ran 18:28:33-18:32:53, covering only radius 0 (r1-r3), radius 25 (r1-r3) and
radius 50 (r1-r2). The remaining four repeats — radius 100 (r1, r2, r3) and radius 50 (r3) — were
produced separately at 19:03:06-19:05:35, a 30-minute gap. All four ran at the same commit and
manifest parameters, so the data is legitimate, but only one of the four (`interestRadius50-r3`) was
previously disclosed as a replacement run. **Whether the other three (radius 100, r1-r3) were
themselves discard replacements is not established — disclosed here as unknown, not resolved.** The
discard log names only `interestRadius50-r3` as an E3 replacement, but that log's completeness for the
other three has not been independently verified against the underlying run history, and this document
does not guess. If they were replacements, "E3's one discarded repeat" below is incomplete; if they
were not, the 30-minute gap has no discard-related cause and is unexplained on its own. Either way the
direction would be **against** the claim, not for it — a higher-throughput radius-100 replacement would
*lower* the radius-100 reduction, which is already the row that undershoots its published figure — so
no conclusion in this document is at stake, only the completeness of this one disclosure. The
consequence that **is** established: **the radius-100 numerator above (measured at 19:03) is divided
against a radius-0 denominator measured 34 minutes earlier (18:29)** — not a within-session comparison,
which is the property every other ratio in this table leans on — and radius 100 is exactly the row that
diverges most from the published figure.

**A discard-bias caveat, correctly scoped.** Ten of the 36 repeats across this phase's three
experiments (both E8 client counts and E3) were discarded for custody firing and individually re-run —
full list in `docs/superpowers/results/2026-08-23-A-instrumentation.md`. The general argument that this
biases results toward the claim (custody fires under load, load depresses tick rate, tick rate sets
snapshot volume, so discarding custody-firing runs preferentially discards low-throughput runs) is
**materially true only for `exp-bytes-2client` radius 0**, where all three repeats are replacements
(stated where that comparison is used, §3 E8 below). It does **not** apply to E3's radius-50 row: E3's
one *known* discarded repeat, `interestRadius50-r3`, is a radius-**50** replacement, so it can only move
the radius-50 numerator — and a higher-throughput replacement there would *lower* the radius-50
reduction, not raise it. (Whether E3's radius-100 repeats are also replacements is the open item above;
if so, the same direction applies there too.) Nor does it apply to `exp-bytes-1client`'s radius-0
baseline used in the cross-check
above: only one of its three r=0 repeats (`r0-r1`, one of 3 of 12 discarded in that experiment) was
replaced, medians are used throughout, and that replacement is the **lowest**-ticksum run in its
radius, so it cannot have raised the baseline. These absolute counts supersede the published
percentages rather than merely confirming their ratios survived; the magnitudes differ substantially
in part for the reasons above, and in part from an unexplained snapshot-throughput change between
measurement sessions that is not attributable to tick-rate variability — recorded as an open item
under E8 (§7 item 13).

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

**Status: measured on counted datagrams — the verdict is build-scoped.** Backlog items 4, 5 and 11 are
closed. Item 10 is closed in the sense that mattered — `net_cli_wire_bytes`/`net_peer_wire_bytes`
(`tools/analyse.py`, backed by ENet's own `GetTotalSentData()`/`GetTotalSentPackets()`) read what was
actually written to the socket **after** ENet coalesces queued commands into datagrams, costed at
payload + 28 B/datagram (IPv4 20 + UDP 8; ENet's own header is already inside `totalSentData`), and the
overhead-model ambiguity is gone because the model itself is gone. **What is not true is that counting
datagrams improved the verdict.** It made the ratio worse (below). **The claim's support comes from
this build**, where it holds comfortably at every radius tested; on the published build, the same
counterfactual test (§ below) is marginal and too close to call at radii 25 and 50, and fails at radius
100. This build's snapshot throughput is higher than the published build's — most sharply at radius 0
(3.6x), least at radius 25 (1.36x) — compounding a shift in interest management's own reduction
fraction (68.1% published -> 87.9% here); neither change is explained (item 13). 2 servers,
4,000 objects, `uniform`, 1 client, 20 s realtime, 3 repeats,
`--halo-width 8`, halo unreliable (deployment-realistic). Full detail in
`docs/superpowers/results/2026-08-19-E8-bandwidth.md` (superseded, see its header note) and
`docs/superpowers/results/2026-08-23-A-instrumentation.md`.

| interest radius | client-facing wire B/s | peer-facing wire B/s | saving vs radius 0 | peer / saving |
|---|---|---|---|---|
| 0 (everything) | 10,008,710 | 1,580,699 | — | — |
| 25 | 1,496,458 | 1,744,768 | 8,512,252 | **0.205** |
| 50 | 2,261,120 | 1,769,330 | 7,747,590 | **0.228** |
| 100 | 5,838,452 | 1,735,561 | 4,170,258 | **0.416** |

Client-facing traffic is monotone in radius. Peer-facing (halo) traffic reads flat within 11.9% across
radius on raw B/s (1,580,699–1,769,330), but that raw comparison has little discriminating power at
n=3: the within-radius repeat spread at r=0 alone is 32% (26.9–35.5 M B across the three repeats),
wider than the 11.9% cross-radius spread of medians it would otherwise be offered as evidence for.
Normalising per tick — the same correction applied to the 2-client data below — removes that noise
instead of merely naming it:

| radius | peer B/tick |
|---|---|
| 0 | 7,886 |
| 25 | 7,952 |
| 50 | 8,068 |
| 100 | 8,074 |

**2.4% spread of medians** — tighter than the published "flat within 4%". But judged by the same
standard applied to the other two flatness checks in this document (compare the cross-radius spread
against the within-radius repeat spread), this table's own within-radius spread is **7.8–12.1%**
(radius 0: 7,211/7,886/8,081, 12.1%; radius 25: 10.4%; radius 50: 7.8%; radius 100: 9.2%) — so the 2.4%
cross-radius spread sits *below* the noise floor, not resolved by it. The result is genuinely better
than the raw check (normalisation cut radius-0 noise from 32% to 12.1%) and is consistent with
independence, bounding any radius dependence to ≲10%; the 2-client per-tick result below, whose 10.3%
residual sits right at (not comfortably under) its own within-radius spread, is consistent with
independence by the same test but is no cleaner a demonstration either. The raw 11.9%-vs-32% comparison
is retired as uninformative rather than kept as the headline check. Columns are named for the host
measured, not the traffic assumed to dominate it — see the results document for the `manifestSent=0` /
`hoSent` bound that confirms snapshots and halo actually do dominate their respective hosts on this
configuration.

**Counting real datagrams made the ratio worse, not better.** A prior pass through this evidence
attributed the published-to-measured drop entirely to "the size of ENet's coalescing effect" — that
attribution is backwards. Applying the retired flat-36-B-per-packet model to *this run's own* snapshot
counts, instead of to the published run's, shows counting datagrams costs the ratio at every radius:

| radius | old model, this run's counts | counted datagrams, this run | effect of counting |
|---|---|---|---|
| 25 | 0.119 | 0.205 | **×1.72 worse** |
| 50 | 0.134 | 0.228 | ×1.70 worse |
| 100 | 0.238 | 0.416 | ×1.75 worse |

This is the correct direction: the published model charged 36 B of header on *every packet*; real
datagrams coalesce many packets together, so true overhead is lower, the byte *saving* interest
management buys is therefore smaller, and the ratio (peer / saving) rises. Counting datagrams is a
penalty the claim absorbs, not a mechanism that helps it — the published document's own payload-only
row (the least-overhead bound) already showed this: it produced the *worst* ratios (1.09/1.11/2.12),
not the best.

The actual coalescing effect, measured directly rather than read off a ratio column that conflates it
with the snapshot-throughput change below: at radius 0, `snapSent` 2,415,460 against `netCliPkts`
89,781 — **26.9 object-snapshots per datagram** — for **40.9 B of real wire per snapshot, against the
68 B the old per-packet model charged**. Counted-vs-modelled total bytes: **0.601 / 0.745 / 0.656 /
0.623** across radii 0/25/50/100. That is what coalescing is worth on this workload.

**That asymmetry is also the answer to the reader's actual question — why the claim is true at all, not
just why the ratio moved.** Snapshots coalesce far more than halo packets do: 26.9 snapshots share a
datagram (40.9 B real wire against 32 B of payload — a **1.28x inflation**), while halo packets
coalesce only about 1.03:1 (16,186 datagrams carrying 16,746 halo packets; roughly 1,124 B real wire
against ~1,096 B of payload — a **1.03x inflation**). Interest management removes the smallest, most
numerous, most heavily marked-up packets; the halo's few large packets are barely marked up at all.
That is weaker than the pre-instrumentation description ("a delta snapshot is 24 B of payload, so
per-datagram overhead nearly triples it") — the real inflation is 1.28x, nowhere near 3x — but it is
the same direction, still asymmetric, and it is the mechanism the claim survives on.

**The published->measured move (0.531 -> 0.205 at radius 25) is not explained by a single "3.6x"
figure** — an earlier version of this document used one, and that presented a radius-0 number as if it
applied everywhere. Per radius, this build's snapshot throughput against the published session's:

| radius | this build / published, snapshots/s |
|---|---|
| 0 | 3.60x |
| 25 | **1.36x** |
| 50 | 2.10x |
| 100 | 3.08x |

Using the **radius-0** throughput factor (3.60x — not the radius-25 factor of 1.36x; applying 1.36x
here would predict ×0.735, not what is observed) as a naive uniform estimate predicts roughly a ×0.29
factor on the ratio move at radius 25; the observed factor is ×0.23. The gap is **not** evidence of an
independent third cause: the radius-25 throughput factor itself (1.36x) is the *product* of the
radius-0 throughput change and a shift in interest management's own reduction fraction, measured
inside this same E8 run, from **68.1% published** (radius 25 retained 31.9% of radius-0 bytes:
1,477,392 / 4,630,270 B/s, the published table's byte column — proportional to snapshot counts under
its flat-overhead model) to **87.9% here** (radius 25 retained 12.1% of radius-0 snapshot counts:
590,953 / 4,897,037) — `3.60 x (0.121 / 0.319) = 1.366`, matching the radius-25 entry in the throughput
table directly. So the published->measured move at radius 25 is better described as a three-term
product: ×1.72 (counting datagrams, against the claim) × ×0.29 (radius-0 throughput, i.e. 1/3.60, for
the claim) × ×0.78 (reduction fraction, for the claim) = **0.389**, against the observed 0.386. The
last two terms are not independent — they share one unexplained cause — which is why item 13 tracks
them together, rather than as three independent multiplicative factors.

Isolating a like-for-like comparison — this run's measured wire-bytes-per-snapshot, applied to the
*published* run's implied snapshot counts rather than this session's own (unaffected by the correction
above; it was already built the right way):

| radius | published (36 B model) | counted datagrams at published throughput |
|---|---|---|
| 25 | 0.531 (holds) | **0.994** — marginal |
| 50 | 0.537 (holds) | **0.940** — marginal |
| 100 | 1.027 (unsettled) | **1.839** — fails |

(If anything this is optimistic for the claim: coalescing is weaker at lower snapshot rates, so the
true published-throughput figures are plausibly higher still. This counterfactual also pairs the
published run's *modelled* halo numerator with this run's *counted* denominator — a small, ~0.8%
effect, that would move the radius-25 figure from 0.994 to roughly 0.986 if counted consistently; too
small to change the "marginal" reading.)

**Verdict: the claim holds at every radius tested, including 100, at a single client, on this build.**
Whether it would also hold on the published build is not established either way, and this document does
not claim it would. The counterfactual above is **marginal and too close to call at radii 25 and 50**
(0.994, 0.940 — within 1% and 6% of the threshold respectively, against a peer-facing column that carries
roughly 10% repeat-to-repeat noise elsewhere in this section, and the estimate is itself optimistically
biased by an unquantified amount, per the note above) and **fails outright at radius 100** (1.839). A
figure that close to a threshold, that noisy, and that biased cannot support "likely holds", and this
document does not use that phrase. **The claim's support comes from the current build, at every radius
tested, not from an inference about the published one.** This is a build-scoped result, not a
strengthened-by-instrumentation one: why throughput and the reduction fraction moved between builds is
itself unexplained and is **not** tick-rate variability — the peer-facing/halo column, which depends
only on tick rate, agrees with the published session to within 2.2% (see item 13 for the full argument).
A same-session counterfactual on 2026-08-24 reproduced that throughput change back to back on one
machine (3.50x at radius 0), confirming it is a build difference rather than session drift, and found
the published build to be **erratic in exactly this quantity** — 130.6% within-radius spread against
this build's 0.3%, with servers intermittently failing to accumulate ticks. The published-build
counterfactual above is therefore a weaker challenge to this verdict than it looks: it is computed on
the less well-behaved of the two builds. It is not thereby overturned, and this section's claim still
rests on the current build alone.
This closes backlog item 10 honestly — the overhead-model ambiguity is gone because the model is gone,
and the claim survives on this build, but not because counting datagrams "improved" anything.

**Client count is now measured, not extrapolated.** `-Clients N` (`tools/run-experiments.ps1`,
`tools/measure.ps1`) starts N clients against a world held constant at 4,000 objects
(`-Objects 2000 -Clients 2` against `-Objects 4000 -Clients 1` — `--objects` is per client, see the
harness-trap note in `CLAUDE.md`). **All three radius-0 repeats of the 2-client experiment were
custody-firing replacement runs** (see the results document's discard log) — that is the baseline every
2-client saving below is computed against. At radius 25: the saving nearly doubles from 1 to 2 clients
(8,512,252 -> 14,460,978 B/s) and the ratio roughly halves (0.205 -> 0.105) — the direction the
published analytical argument predicted, now measured.

Peer-facing bytes at 2 clients read flat within only 38.0% across radius (1,173,961–1,619,664 B/s),
wider than the 11.9% at 1 client — but this is a systematic, explained effect, not noise. The halo band
publishes once per tick, and radius 0's much heavier client-facing load costs the servers tick rate, so
it accumulates fewer ticks in the 20 s window than the other three radii:

| radius | ticksum | peer B/s | peer B/tick |
|---|---|---|---|
| 0 | 3,082 | 1,173,960 | 7,618 |
| 25 | 4,383 | 1,513,247 | 6,905 |
| 50 | 4,388 | 1,619,664 | 7,382 |
| 100 | 3,996 | 1,399,455 | 7,004 |

Normalised per tick, the spread is **10.3%** (9.6% if peer bytes and ticksum are paired per-repeat
rather than each taken as the median across repeats — a pairing note, not a different conclusion), not
38.0%. Halo/radius independence — the mechanism E8's whole claim rests on — **holds, by the same
standard the 1-client data above is judged by**: this 10.3% residual is **no larger than its own
within-radius repeat spread at r=0** (10.2%: 6,953 / 7,660 / 7,554 B/tick) — the identical
sample-size argument that retires the raw 1-client comparison as uninformative says this residual is
consistent with pure independence, not merely "small enough to call conservative." Separately, and
still true on its own: because the r=0 denominator under-accumulates ticks from the same tick-rate
effect, the saving at radius 25 is *understated* relative to what an equal-tick-rate comparison would
show, so the measured **0.105 two-client ratio is conservative** on top of that — a point in the
claim's favour, recorded here rather than left as an unresolved flag.

The retracted 25.7 / 33.9 MB/s figures were wrong by a factor of ~16. The "explicitly not a
measurement" geometric estimate recorded alongside them, ~1.6 MB/s, is close to both the originally
measured 1.615 MB/s and this re-measurement's 1.58–1.77 MB/s peer-facing range.

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

  **What the single machine does and does not hide (2026-08-21).** Splitting the per-tick cost into
  simulation and coordination separates the two, and they behave differently. On a fixed total load
  (injection at 160/s, 4,800 objects, 3,600 paced ticks):

  | servers | tick period | physics | coordination overhead | objects/server |
  |---|---|---|---|---|
  | 1 | 14.01 ms | 9.87 ms | 3.94 ms | 4,799 |
  | 2 | 9.23 ms | 4.39 ms | 4.72 ms | 2,400 |
  | 4 | 9.05 ms | 2.34 ms | 6.57 ms | 1,200 |

  The **physics scales very nearly linearly** — 2.25x then 1.88x per doubling — and that half is not
  contention-limited, because it is per-server work on a shrinking share of the world. The
  **coordination overhead grows** with server count, so 2 -> 4 servers saves 2.05 ms of physics and
  spends 1.85 ms more coordinating, and the total barely moves.

  That flat segment is the part the single machine confounds: four servers plus the manager, midware
  and client are seven processes on six cores, so the overhead term carries contention as well as
  genuine per-peer protocol cost. The two are not separated here. Supporting both readings at once:
  `haloLate` improves with server count (23.0% -> 14.2%), which is lower per-server load helping,
  while inter-server tick drift worsens (20 -> 105 ticks), which is contention making progress
  uneven. **Do not quote the 2->4 segment as a scaling limit of the design** - it is a measurement of
  this machine. The 1->2 segment (50.4 s -> 34.8 s wall for the same simulated work) is the safer
  figure, and the physics column is the safest of all.
- **No latency injection.** Every measurement runs on a local network with effectively zero link
  latency. Nothing here says how any of these claims hold up once cross-server or client-server
  messages carry real network delay.
- ~~**No AP-comparable injection workload.**~~ **Closed.** `--workload injection` reproduces Aura
  Projection's published benchmark (160 objects/s for 60 s), measured at 1 and 2 servers with a
  4-server scaling point. See `docs/superpowers/results/2026-08-21-AP-injection.md`, which also
  carries the declared-deviation table without which "AP-comparable" means nothing. Comparisons to
  Dyconits-style systems still lack a shared workload definition.
- **Tick-epoch divergence bounds cross-border fidelity under load.** Halo scheduling is expressed in
  the *sender's* tick numbers, and two servers share no epoch once either stops holding its pacing
  budget. On the full AP load the counters diverge monotonically to 89 ticks, and the server running
  ahead sees 84% of halo arrivals already past due. Invariant **I8 is unattainable while that
  persists**, since the lookahead exists precisely so both servers apply an update on the same
  simulated tick. The withholding half is now bounded (`haloAhead`), but the divergence is not cured
  and cannot be at that layer: it needs both servers inside their pacing budget. Any cross-border
  figure taken at 9,600 objects on one machine carries this.
- **`headon` is one synthetic workload.** E2 and E5's soundness sweep both rely on `headon` — pairs on
  fixed lanes, one speed, meeting the border perpendicular — which is the configuration in which the
  halo's knee is sharpest and easiest to locate. Oblique approaches, mixed speeds, and denser traffic
  would stress the bound harder and are untested.
- **E8's bandwidth verdict is build-scoped, and absolute realtime byte/snapshot counts do not compare
  across measurement sessions.** §3 E8 holds on the commit it was measured on (`2060f55`); whether it
  holds on the published commit (`02e306b`) is untested, and the counterfactual there shows radius 100
  specifically would not hold without this build's throughput and reduction-fraction changes — though
  that counterfactual is computed on a build since measured to be erratic by up to 130% in this very
  quantity (§7 item 13, 2026-08-24), so it bounds the verdict less tightly than its arithmetic suggests.
  Those changes are open and unexplained (§7 item 13), confirmed not to be tick-rate variability, so a reader
  should not treat the 10.0 MB/s vs. 4.63 MB/s radius-0 client-facing gap, or any other absolute
  realtime rate, as comparable **across** sessions on this evidence — only the ratio figures computed
  within one session's own repeats are load-bearing.

---

## 6. Known conditional guarantees

**Ownership atomicity holds only while both servers keep pace, and only above `--handoff-lookahead
0`.** The B6 mechanism makes ownership transfer atomic by having the sender release at the same
`senderTick + lookahead` tick the receiver installs on — but only when both servers reach that tick
at comparable real times, and only when a non-zero lookahead is configured at all. The default is 0,
where the sender releases on send and the gap is unconditional, not an edge case (§7 item 2). A
rebalancing migration violates the keep-pace condition by definition, because the partition is, by
construction, still imbalanced during the move: E4 measured ~2,538 median ownership-gap ticks per
dynamic run, tracking `hoLate` almost exactly, with conservation exact and nothing lost — that
measurement predates the custody work below. E7 found the same mechanism under sustained overload
rather than a transient migration, and there the consequence was worse: past the ~3,384 objects/server
correctness budget, gaps become near-permanent (1,795-1,798 of 1,800 ticks) and, before custody,
objects were actually lost (`conservation_delta` -4 to -8 at 8,000 objects).

**Custody closes the loss half of that consequence, not the gap itself.** The handoff-custody work
(§7 item 1) makes a sender hold the transfer packet until the receiver acks it, so a dropped ack no
longer costs an object outright: at 8,000 objects `conservation_delta` moved from -4/-8/-8 (silent
loss, no custody) to -15/0/-4 (`runs/exp-fix4-E7`), and the residual is traceable rather than
unaccounted — `ho_parity_delta` matches it exactly on every repeat, and it reads as end-of-run
truncation of transfers still in flight, not disappearance. That is not a general "gaps stopped
mattering" result, though: the rebalancing case regressed rather than improved over the same window.
`runs/exp-fix4-E4` loses 84 to 703 objects where the pre-custody baseline (`runs/exp-balance`, commit
`93e6f21`) was exact, and that regression has not been isolated from the other changes made across the
same period — see §7 item 12. The ownership guarantee is real but conditional, never unconditional,
and the experiments above independently expose the same underlying failure mode from three different
angles: a transient migration, sustained overload, and now custody's own interaction with rebalancing.

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
(§4.1). Items 1, 2, 6 and 7 were addressed together as **Batch B**, followed by one full
re-measurement (`docs/superpowers/results/2026-08-20-B-custody.md`). Item 1 is fixed, item 6 is
withdrawn as a mis-filed defect, and items 2 and 7 stay open — narrowed and confirmed-reachable
respectively, not closed. Batch B's own measurement also surfaced a new item, 12. Items 10 and 11 were
closed by the Phase A instrumentation work (`docs/superpowers/results/2026-08-23-A-instrumentation.md`),
which is also where the E8 and E3 re-measurements in §3 above come from. That same re-measurement
opened item 13: an unexplained, radius-dependent change in E8's snapshot throughput (3.6x at radius 0,
1.36x-3.08x elsewhere) between the published commit and this one, plus a second, equally unexplained
shift in interest management's own reduction fraction — shown not to be tick-rate variability.

1. ~~**Handoff ack is stubbed.**~~ **Fixed.** The receiver acks on acceptance and the sender holds the
   transfer packet in custody until the ack arrives (`CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h`,
   `ServerWorldManager::FlushPendingTransfers`), resending on a wall-clock deadline and reclaiming the
   object only once the peer link itself is gone — never on a bare timeout, which the batch found could
   not distinguish "never arrived" from "slow to arrive" (below). At 8,000 objects, 2 servers, `uniform`,
   halo on, `--handoff-lookahead 0`, `runs/exp-fix4-E7` measured `conservation_delta` of -15, 0, -4
   against a -4, -8, -8 baseline with no custody — an object can no longer vanish outright because a
   receiver refused or died. The residual is addressed under item 2, not claimed as zero here.
2. **Ownership atomicity fails above the tick budget — narrowed, not closed.** Custody (item 1) makes a
   transfer lossless in the sense that nothing disappears silently: an outstanding transfer stays
   visible as `hoCustody` and is reported by `analyse.py`, and the residual E7 loss (-15/0/-4) is
   end-of-run truncation of transfers still in flight, not an unaccounted loss — `ho_parity_delta`
   matches it exactly on every repeat, `hoPending`/`hoSched` read 0, and `hoCustody` is at least the
   loss on each one.

   **Note the deliberate tension here: the same non-zero `hoCustody` that is cited as evidence above
   is reported as an INVARIANT FAILURE by `tools/analyse.py` (`check_custody`).** That is intended,
   and the check is not weakened. `hoCustody > 0` at exit means a transfer was still outstanding when
   the server stopped, and an outstanding transfer *is* an unaccounted object — the gate takes the
   conservative reading and refuses to call such a run clean. What custody bought is not a passing
   gate but a *visible* one: before custody the same objects vanished with every counter reading 0,
   so the run passed. The two statements are therefore consistent — `hoCustody` explains *where the
   loss went* (still in the sender's hands, recoverable, not gone), which is exactly why it is
   admissible as evidence; it does not certify the run as loss-free, and the analyser is right to say
   so. A run intended to *pass* the gate must be drained (`--drain-seconds`) until `hoCustody` reads
   0; the E7 runs quoted here were not, and are reported as failures by the shipped analyser.

   What custody does not touch is the ownership *gap* itself: at `--handoff-lookahead
   0` — the default, and what most experiments in this document ran at — `ScheduleOutgoingObject`
   releases on send exactly as before, so nobody owns the object for one network round trip. The
   atomicity guarantee stays conditional on `--handoff-lookahead > 0`, the non-default case (§6).
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
6. ~~**Load profile buckets objects, not contacts.**~~ **Withdrawn — this is a deliberate design
   decision, not a defect.** `TakeLoadReport` states the reason in place: a contact belongs to two
   objects that may fall in different buckets, so charging it to either one is arbitrary, while
   object count within a bucket is a sound proxy because contact cost scales with local density.
   What remains true is the *consequence*, and it stays recorded as a limitation in §6: the balancer
   equalises objects, so E4 ends with near-perfect object balance and a residual contact imbalance
   (14.2M vs 8.1M). That is a stated property of object-count balancing, not an unfixed bug.
7. **`CalculateIncomingObjectOffsetPosition` is never called — confirmed reachable, stays open.**
   `ServerWorldManager.cpp:440`. Incoming handoffs still get no positional nudge into the receiving
   region. Batch B added `hoClamp`, a counter that computes the clamp this function would apply and
   discards it without changing behaviour, purely to observe whether the gap is real. It fired 25,434
   times across 10 of 52 server-lines on the E7 runs, and roughly 2,600 times per run under
   rebalancing. **It also fires on the correctness-budget (healthy-path) configuration when
   `--halo-width` is on**: server 0 reported `hoClamp` 7–8 across the four Phase A pre-change repeats
   and 8, 8, 8 across the three gate repeats — see the Step 4 table in
   `docs/superpowers/results/2026-08-23-A-instrumentation.md:152`. The likely mechanism is that an
   arriving object is promoted from an existing halo shadow rather than constructed, so its arrival
   position comes from a different path; every earlier run behind the "never on healthy-path" claim
   had the halo off. It fires with
   `hoReclaimed = 0`, so it is not an artefact of custody's own reclaim churn. That refutes the
   expectation, held when this item was filed, that the gap was practically unreachable: it is real
   and load-dependent, and reachable at correctness-budget scale, not only under load or rebalancing.
   The fix (wiring the function into the handoff path) stays deferred for the same
   reason as before — it changes measured handoff behaviour, so it belongs with a dedicated change,
   not a drive-by.

   **The function's body is also wrong, not merely unwired** (found 2026-08-23). It clamps Z with an
   *inclusive* upper bound, and its comment claims the bounds "mirror `IsObjectInBorder` exactly —
   half-open on X, closed on Z". That was true before the ownership unification; `IsObjectInBorder`
   now delegates to `OwningServerFor`, which is half-open on **both** axes. So on an interior Z seam
   the clamp returns a position this server does not own — the disowned-object case the unification
   exists to prevent. Masked at 2 servers (1-D split, `maxZ == worldMaxZ`, closed-outer-edge
   exception) and reachable at 4 (`CalculateServerBorders` builds a 2×2 grid); inert today only
   because the result is discarded. The Z bound must be fixed *before* the function is wired in, and
   the stale comment corrected with it. See
   `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` §5.3.
10. ~~**Bytes are counted as packets, not datagrams.**~~ **Fixed, with a corrected interpretation.**
   `net_cli_wire_bytes` and `net_peer_wire_bytes` (`tools/analyse.py`) now read ENet's own
   post-coalescing counters (`GetTotalSentData()`/`GetTotalSentPackets()`,
   `DistributedGameServerManager::GetNetworkByteTotals()`) and cost each real datagram at payload + 28 B
   (IPv4 20 + UDP 8; ENet's own header is already inside `totalSentData`), replacing the flat-36-B-
   per-packet model entirely. **Counting real datagrams makes the ratio worse, not better** —
   coalescing means less overhead than the per-packet model charged, so a smaller saving and a higher
   ratio; applied to this run's own counts, counting costs the ratio ×1.7–1.75 at every radius (§3). The
   measured ratio (0.205–0.416 across radii 25/50/100, comfortably under 1.0 including radius 100)
   reflects this build. Whether it would hold on the published build is not established: the
   counterfactual at published throughput is marginal and too close to call at radii 25/50 (0.994,
   0.940 — within 1% and 6% of the threshold respectively, against ~10% repeat noise, and itself optimistically
   biased) and fails at radius 100 (1.839, §3). The claim's support comes from this build, where it
   emits more snapshots than the build the published 0.531–1.027 figures came from — 3.6x at radius 0,
   radius-dependent down to 1.36x at radius 25 (§3) — compounding a shift in interest management's own
   reduction fraction. Neither is explained, tracked together as item 13, not part of what this item
   closes. What this item closes is the overhead-model ambiguity itself: the model is gone, replaced by
   a direct read of what ENet wrote to
   the socket. See §3 for the full decomposition.
11. ~~**The harness starts one client.**~~ **Fixed.** `-Clients N` (`tools/measure.ps1`,
   `tools/run-experiments.ps1`) starts N clients. E8's client-count scaling is now measured rather than
   extrapolated: at 1 vs 2 clients (world held constant at 4,000 objects, `objPreseed=4000` confirmed on
   both), radius-25 saving nearly doubled (8,512,252 -> 14,460,978 B/s) and the peer/saving ratio roughly
   halved (0.205 -> 0.105) — the direction the analytical argument predicted. All three radius-0 repeats
   of the 2-client experiment were custody-firing replacement runs (§3), and the 2-client peer-facing
   spread (38.0% raw) is a per-tick-rate effect, not noise: normalised to B/tick it is 10.3%, halo/radius
   independence holds, and because the r=0 baseline itself under-accumulates ticks from the same effect,
   the 0.105 ratio is conservative rather than an open question. See §3.

   **Qualification (F4, found 2026-08-23; resolved 2026-08-24):** no client `@@FINAL` line was printed
   in any run of this phase, so this item's per-client discovery (numbered `cli-N.log` files), though
   real and unit-tested, had only ever been exercised against fixture data. The harness now ends the
   client on link loss and waits for its line; a 2-server run on 2026-08-24 reconciled a real client
   `cmdSent=251` against the servers' 244+7+0-0 exactly. The discovery path is therefore now exercised
   against real client data. See item 14.
13. **Narrowed 2026-08-24, still open — a snapshot-throughput change (not tick-rate variability), plus a second, distinct change in
   interest management's own reduction fraction — both between the published commit and this phase's
   re-measurement.** Client-facing byte rates measured in this phase are 2-4x the published ones at the
   same nominal configuration (radius-0 client-facing: 10.0 MB/s here vs. 4.63 MB/s published). The
   underlying object-snapshot throughput change is **not uniform across radii** — 3.6x at radius 0, but
   only 1.36x at radius 25, 2.10x at radius 50 and 3.08x at radius 100 (§3) — an earlier version of this
   item described it as a flat 3.6x, which was a radius-0 figure presented as global. The obvious
   explanation for any of this — an unpaced `--run-seconds` run simulating a different amount of work
   depending on machine load — is refuted by the peer-facing (halo) column in the same tables: published
   halo throughput was 1,615,376 B/s, re-measured 1,580,699 B/s, **within 2.2%**; implied halo packets/s
   were 1,438 published, 1,432 re-measured, **within 0.4%**. The halo band publishes once per tick and
   its own cost model is accurate to ~1% against the counted figures (17.8 entries/packet; 1,096 B
   counted vs 1,096 B modelled payload, 27 B vs 36 B header) — so halo throughput is a direct, accurate
   proxy for tick rate, and it says tick rate is essentially unchanged between the two sessions. A
   near-identical tick rate cannot produce a 3.6x change in snapshot volume, so something in the
   **snapshot path** itself changed between the published commit `02e306b` and this phase's `2060f55` —
   roughly 18 core commits, including custody, halo scheduling, a halo performance fix and a peer-link
   rebuild.

   **A second, equally unexplained factor sits alongside the throughput change**: at radius 25, a
   uniform 3.6x throughput rise would predict a ×0.29 factor on the published->measured ratio move: the
   observed factor is ×0.23. The residual ~×0.78 is interest management's own reduction fraction inside
   this E8 run moving from **68.1% published** (radius 25 retained 31.9% of radius-0 bytes: 1,477,392 /
   4,630,270 B/s) to **87.9% here** (radius 25 retained 12.1% of radius-0 snapshot counts: 590,953 /
   4,897,037) — a change with no more explanation than the throughput shift, and tracked here alongside
   it rather than separately.

   This is recorded here as an **open, unexplained behavioural change**, not as measurement noise, and
   it is why §3's E8 verdict is stated as build-scoped rather than as a straightforward improvement (§3).
   The resolving experiment is a
   single `--run-seconds 20` sweep at commit `02e306b`, using this phase's counted-datagram
   instrumentation (which did not exist at that commit) — that one run would settle items 10
   and this one together. Investigating it further was out of scope for this task.
12. **Rebalancing conservation regression, not attributed to Batch B.** `runs/exp-fix4-E4` (`cluster`,
   4,000 objects, 7,200 ticks, `--handoff-lookahead 300`, rebalancing on) loses 84 to 703 objects across
   3 repeats (-198, -703, -84), where the pre-Batch-A baseline `runs/exp-balance` (commit `93e6f21`) was
   exact. `ho_parity_delta` matches the loss exactly on each repeat and transfers are still held in
   custody at exit, so this is not the silent-loss mechanism items 1/2 describe closing — something else
   is going on. **This regression is explicitly not attributed to Batch B**: the baseline predates Batch
   A too, so the comparison spans the halo-publish rate gate (item 4), the `--drain-seconds` fix (item
   5), and every custody change (items 1/2) at once — any of them, alone or in combination, could be the
   cause. Isolating it needs a bisect of this same configuration at `776115b`, `02e306b`, and HEAD. That
   bisect is the next step, not yet done.
8. ~~**Stale comments in frozen source.**~~ **Fixed.** `NetworkObject.h` now states 60 bytes per halo
   entry (as `PacketSizeTests` measures) and the snapshot gate comment states 60 Hz.
9. ~~**`run-experiments.ps1`'s `-OutDir` is not anchored to the repo root.**~~ **Fixed.** Both scripts
   now share `tools/RunPaths.ps1`, which also rejects the drive-relative case (`C:runs`) that
   `Path.IsPathRooted` reports as absolute. Shared rather than copied precisely because this bug
   existed only because the earlier fix was applied to one script and not the other.

   **Counterfactual, 2026-08-24 — the change is real; the cross-session doubt is retired.** Both
   builds were compiled Release from clean checkouts and swept **back to back in one session on one
   machine** at this experiment's exact configuration. Published->current snapshot throughput:
   **3.50x** at radius 0, 1.81x at 25, 1.91x at 50, **3.15x** at 100, against the 3.6/1.36/2.10/3.08
   recorded above — three of four within 0.1. The inference from two separate sessions was therefore
   sound, and the halo-throughput argument that propped it up is no longer needed.

   **It is in the snapshot path.** At **1 server with `--halo-width 0`** — no halo band, no peer link,
   no handoff, and `snapSupp` = 0 on both builds, so nothing is filtered — the effect survives at
   **3.17x**. It also survives on a **stationary** workload (2.66x), which rules out the obvious
   "deltas now carry the changes they always should have" explanation. The current build ran *fewer*
   ticks than the published one in that control while sending 2.8x more, so it is not tick rate.

   **The current build meets a schedule the published one misses.** Across every configuration tested
   the current build emits **exactly 2000.0 object-snapshots per counted tick**; the published build
   emits 630-755, varying with load and between repeats. The published build's within-radius spread is
   **130.6%** at radius 0 against the current build's **0.3%**, and its server-1 tick counts collapse
   to 513/757/305/1,001 against a normal ~2,390 in the same experiment. That is what drives the one
   radius (25) where the ratio disagrees with the recorded figure.

   **Consequence for E8.** The counterfactual E8's verdict is hedged against — that the published
   build would have failed at radius 100 — is computed on a build that is erratic by up to 130% in
   exactly the quantity E8 measures. It is a weaker challenge to the verdict than it appeared when both
   builds were assumed equally well-behaved.

   **Still open: which commit, and whether it is a regression or a fix.** The snapshot cadence line,
   `BroadcastSnapshot`, the `mServerSideLastFullID` delta baseline and the counting site
   (`mSnapshotsSent += targets.size()`) are all byte-identical between the two commits, so the cause is
   not visible in a diff of the obvious files. Locating it needs a bisect over the **36 non-doc commits**
   in the range (the range holds 61 commits in total; this item's "roughly 18 core commits" understates
   it). Cheapest bisect signal: the 1-server, `--halo-width 0`, radius-0 control — 2000.0 snap/tick or
   not. Full write-up and raw figures:
   `docs/superpowers/results/2026-08-24-item13-counterfactual.md`.

14. **Fixed 2026-08-24 - no client `@@FINAL` line existed in any run of this phase (F4, found
   2026-08-23).** Every client was force-killed before it could print `@@FINAL`. Checked across every
   experiment of this phase - 60 client logs - zero contained `@@FINAL role=client`. Two consequences:
   invariant I4 was silently vacuous in every run rather than failing, and item 11's per-client
   `@@FINAL` discovery had never been exercised against real client data, only unit-test fixtures. E8
   and E3 were unaffected - both read server-side counters only.

   **The diagnosis in the original finding was half right.** The sizing is not an accident to be
   corrected: `measure.ps1` sizes the client's window *past* the servers' on purpose, because a client
   that stops first leaves the servers broadcasting reliable packets at a peer that no longer
   acknowledges them, which blocked their loop for 8.0 s and then 26.0 s on a measured 1200-tick run.
   The bound was never the problem - a bound is the wrong instrument for a role whose natural lifetime
   is "as long as its peers".

   **Fix, in three parts.** `HeadlessRunOptions::stopWhen` ends a headless run early and cleanly on a
   predicate; the client supplies `DistributedMultiplayerGameScene::AllServerLinksLost()`, true once
   the game has started and ENet has reported every physics-server link gone; and `measure.ps1` waits
   for one `@@FINAL role=client` per client, bounded at 45 s, before force-killing anything. The
   ordering the servers need is preserved - the client still outlives them - but it now ends under its
   own power with its totals printed. The 45 s allows for ENet's `ENET_PEER_TIMEOUT_MINIMUM` of 5 s:
   `enet_host_destroy` does not notify peers, so link loss is detected by timeout.

   **Verified on a real run, not fixtures** (2026-08-24, 2 servers, 400 objects, 25 s, `--halo-width 8`,
   `--impulse-test 20 --misroute-every 5`): client `cmdSent=251`; servers `cmdApplied` 177+67=244,
   `cmdRejected` 3+4=7, `cmdDup=0`, `cmdFanout=0`; 244+7+0-0 = **251, I4 balances exactly**, with 47
   relay hops exercised. A companion no-driver run - the shape that produced zero client finals
   throughout Phase A - printed `@@FINAL role=client cmdSent=0 replicas=400 evicted=0 tombstones=0
   resurrectAttempts=0`.

   **`analyse.py` no longer lets an unevaluated I4 read as a pass.** `cmd_delta` is
   `sent - (applied + rejected + dup - fanout)`; with no client line `sent` is 0, and on a run that
   drove no commands the server terms are 0 too, so the delta was 0-0 and reported as a pass while
   checking nothing. Two new keys (`client_finals`, `cmd_server_side`) separate "checked and balanced"
   from "nothing to check". Server-side command activity with no client line is now a **failure** named
   for its actual cause rather than a large negative delta; no commands anywhere is reported as **NOT
   EVALUATED**, and the summary line drops its "command accounting" claim when nothing was reconciled.
   Three unit tests pin this (`I4VacuityTests`), verified adversarially: hardcoding `client_finals`
   breaks two of them.

**Items 3, 4, 5, 8 and 9 are now closed** (Batch A). Item 1 is now also closed and item 6 withdrawn
(Batch B). Item 14 - a harness defect found while writing up item 11, not attributable to any task's
code change - was **closed on 2026-08-24**, which also made invariant I4 non-vacuous for the first
time. Items 2 and 7 remain open - narrowed and confirmed-reachable respectively, not fixed - and Batch
B's own measurement opened item 12, the rebalancing regression, which still needs a bisect.

**E8 has since been re-run** (`runs/exp-bytes-1client`, `runs/exp-bytes-2client`) and is reported in
§3. It closes items 10 and 11 above, and — because the two builds' snapshot throughput differs by a
radius-dependent factor (up to 3.6x) that tick rate cannot explain, alongside a second unexplained
shift in interest management's own reduction fraction — opens item 13, left honestly unresolved rather
than filed as noise. A same-session counterfactual on 2026-08-24 confirmed that difference is real and
localised it to the snapshot path; which commit caused it, and whether it is a regression or a fix,
remain open.
