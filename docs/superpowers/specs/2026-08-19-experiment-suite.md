# The experiment suite (2026-08-19)

Every performance figure produced during the increments was a **single run of a bespoke
configuration**. The repository had 204 ad-hoc run directories and 4 experiment directories, all four
predating the work they were supposed to describe. `run-experiments.ps1` exists to prevent exactly
that and insists on repeats in its own header, and it had been used for none of it.

This records the suite that replaces those figures, and what changed to make it expressible.

---

## 0. What the harness could not do

`run-experiments.ps1` swept `servers`, `objects` and `ticks`, and forwarded none of the flags added
since: halo width, interest radius, physics threads, rebalance interval, world bounds, epoch
alignment. So none of the recent claims could be run through it at all.

It now sweeps those too, forwards every flag, and records all of them in the experiment manifest —
not only the swept one, because several of them decide whether a run is reproducible at all.

`analyse.py` needed three changes:

- **Tag parsing.** Sweep names are camelCase now (`interestRadius`) and values can be fractional or
  negative — a halo width is a distance. The runner encodes `.` as `p` and `-` as `m`.
- **The metrics the increments are about.** Snapshot volume, contacts and halo lateness were not in
  the summary, so a report could not answer the question the experiment was run to ask.
- **A busiest-server table.** An average over servers hides precisely the imbalance partitioning
  exists to fix; the simulation runs no faster than its slowest participant.

**Tick-epoch alignment.** Reproducible runs leave `--epoch-align-us` at 0, measured in
`docs/superpowers/results/2026-08-23-A-instrumentation.md`. Every E1–E8 figure was
taken with it off, so a run that enables it is not directly comparable to those
without re-measuring the baseline.

### One check was wrong, and the experiments found it

The per-tick ownership check added with B6 sums `owned_objects` across servers at each tick. That is
only meaningful when the run is **paced**. In realtime mode each server advances its own counter as
fast as it can, so server A's tick 500 and server B's tick 500 are different simulated moments, and
summing across them compares unrelated instants. The interest experiment — the first realtime one
run through `analyse.py` — reported **2,392 ownership gaps that did not exist**.

The check is now skipped, not weakened, for realtime runs. Paced runs are still checked.

---

## E1 — Locality (I6)

**Claim.** Per-server state scales with region occupancy, not world size.

The world and the object count grow *with* the server count, so objects per region is held constant.
Without that (`-ScaleWorldWithServers`) adding servers only subdivides a fixed world, and per-server
state falls simply because each region got smaller — arithmetic, not a property of the design.

`uniform`, 1,800 paced ticks, 3 repeats:

| servers | world objects | per-server owned (median) | max / min |
|---|---|---|---|
| 1 | 400 | 400 | 400 / 400 |
| 2 | 800 | 400 | 405 / 395 |
| 4 | 1,600 | 400 | 404 / 397 |

**The world grows 4x and per-server state does not move.** All invariants hold on all 9 runs.

---

## E2 — Cross-border collision

**Claim.** Objects either side of a region border collide only when the halo band is on.

`headon` — pairs launched at each other across `x = 0`, so every collision recorded is a border
collision. 100 objects, 1,800 paced ticks, 3 repeats:

| halo width | border crossings, per repeat | contacts (median) |
|---|---|---|
| 0 | **100, 100, 100** | 76,600 |
| 8 | **0, 0, 0** | 82,390 |

Without the halo every pair passes through its partner and is handed to the other side. With it,
nothing crosses at all — the pairs collide and bounce, as they do on a single server. Identical
across all three repeats in both configurations, which is what a deterministic acceptance test
should look like.

The contact total is *higher* with the halo (82,390 vs 76,600) because each border contact is
resolved on both servers by design.

---

## E3 — Interest management

**Claim.** A client's snapshot cost is a property of its view, not of the world.

4,000 objects, 2 servers, 20 s realtime, 3 repeats:

| interest radius | object-snapshots sent (median) | reduction |
|---|---|---|
| 0 (everything) | 1,317,106 | — |
| 100 | 589,128 | **55.3%** |
| 50 | 374,819 | **71.5%** |
| 25 | 277,500 | **78.9%** |

Monotone in radius, as it must be. Note the baseline here already excludes snapshots to peer
servers, which was a separate and larger saving (they register no handler for snapshots at all).

> Realtime rather than paced, deliberately. Snapshot volume is a rate, so it has to be measured
> against wall-clock time; a paced run would report whatever the pacing allowed.

---

## E4 — Dynamic load balancing

**Claim.** Moving the borders at runtime reduces the cost borne by the busiest server.

`cluster` — objects packed into one part of the world and staying there, which is the only workload
that can judge a balancer. 4,000 objects, 2 servers, 7,200 paced ticks, 3 repeats:

| rebalance interval | busiest p95 ms | objects | busiest : lightest |
|---|---|---|---|
| 0 (static) | 11.83 | 4,000 / 0 | **8,452 : 1** |
| 400 ticks | **10.50** | 2,007 / 1,993 | **1.71 : 1** |

Object balance is essentially perfect. Conservation exact on every run.

**The busiest-server improvement is 11%, not the 30% a single run suggested.** The single run
measured wall clock over the whole run, which includes the interval before the partition converged
and is dominated by it; the p95 per-tick figure here is the steadier measure and the honest one. This
is exactly what repeats were supposed to catch.

### The gaps this experiment exposed

Every dynamic run reported ownership gaps — 2,574, 1,891 and 2,538 ticks — and they are real, not an
artefact like E3's:

| | median |
|---|---|
| `hoLate` | 2,375 |
| ownership gap ticks | 2,538 |
| conservation delta | **0** |

The two track each other almost exactly, and conservation is exact, so **nothing is lost**. What
happens is that a transfer arrives after the tick it was scheduled for, so the receiver installs it
late: the sender released at tick T, the receiver installed at T', and for the ticks between them the
object was owned by nobody.

The cause is the pacing skew that runs through this whole system. B6 makes ownership transfer
atomic *provided both servers reach the agreed tick at comparable times*. An overloaded server does
not — and during a rebalancing migration the partition is, by definition, still bad. So the
mechanism that closes the ownership gap is defeated by exactly the condition that makes rebalancing
necessary in the first place.

That is worth stating rather than tuning away. It means the atomicity guarantee is conditional on
load balance, and the two increments are less independent than they looked.

---

## What this suite still cannot show

**Multi-machine.** Every run puts every server, the manager, the midware and the client on one
6-core machine — now recorded in each experiment manifest, alongside the core count, precisely
because it is the reason a wall-clock comparison across server counts measures contention rather
than distribution. E1 is safe from this (it measures state, not time) and E2 is safe (it measures
events). E4's timing figures are not: they compare two partitions of the *same* total work on the
same hardware, which is a fair comparison, but they are not a distributed speedup.

Turning any of this into a speedup claim needs one server per machine. Nothing in the code has to
change for that — only the run configuration.
