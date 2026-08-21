# Workloads

A **workload** is the test scenario: where objects start, how they move, and whether new ones
appear during the run. You pick one with `-Workload NAME`.

The workload decides what a run can prove. Choosing the wrong one is the most common way to
produce a number that looks meaningful and is not.

---

## Quick guide

| Workload | Objects start… | They move… | Use it to show… |
|---|---|---|---|
| [`uniform`](#uniform) | spread across the whole world | sideways, crossing boundaries | balanced, general-purpose behaviour |
| [`shuttle`](#shuttle) | all in one region | sideways, crossing boundaries | the worst case for a fixed split |
| [`cluster`](#cluster) | packed into one area | barely | whether load balancing works |
| [`headon`](#headon) | in pairs either side of a boundary | straight at each other | cross-boundary collision |
| [`seam`](#seam) | centred exactly on the boundary lines | normally | that the ownership rule is exact |
| [`injection`](#injection) | nowhere — the world starts empty | falling and scattering | behaviour under a growing world |

**If you are unsure, use `uniform`.**

---

## `uniform`

Objects are spread evenly across the whole world and given sideways velocity, so they cross
boundaries steadily throughout the run.

This is the balanced, general-purpose scenario. Each server starts with roughly the same amount
of work, so timing comparisons between servers are fair.

```powershell
-Workload uniform -Objects 400
```

**Use it for:** most performance work, scaling comparisons, anything where you want the split to
be even.

---

## `shuttle`

Same motion as `uniform`, but every object starts inside **one** region.

This is deliberately unfair. With a fixed split, one server begins with about 90% of the world
and the other sits nearly idle. It is the worst realistic case for a static partition.

```powershell
-Workload shuttle -Objects 400
```

**Use it for:** showing what happens when a fixed split is badly matched to where objects
actually are.

> **Warning.** Because the load is so uneven, the two servers run at very different speeds. That
> makes anything depending on them staying in step — particularly cross-boundary collision —
> unreliable. Do not use `shuttle` together with `-HaloWidth` and expect trustworthy results.
> Report `uniform` and `shuttle` together: they bracket the range.

---

## `cluster`

Objects are packed into one part of the world and **stay there**.

This is the only honest way to test load balancing. `uniform` is already balanced, so a balancer
has nothing to do. `shuttle`'s load moves as fast as an object can travel, so a balancer can
never catch up. `cluster` is unbalanced but stationary — exactly the case a balancer should fix.

```powershell
-Workload cluster -Objects 4000 -RebalanceInterval 400
```

**Use it for:** load balancing experiments, comparing a fixed split against a moving one.

---

## `headon`

Objects are launched in pairs from either side of the boundary at `x = 0`, straight at each
other, one pair per lane.

Every collision in this scenario is a collision *across a boundary*. That makes it the acceptance
test for cross-boundary collision: with the feature off, the pairs pass straight through each
other; with it on, they collide.

```powershell
-Workload headon -HaloWidth 8
```

**Use it for:** proving cross-boundary collision works, and finding the smallest band width that
still catches every contact.

> On a single server nothing crosses a boundary, so this scenario proves nothing at
> `-Servers 1`. It needs at least two.

---

## `seam`

Objects are arranged so a whole row and column sit **exactly** on the lines `x = 0` and `z = 0`.

Boundaries are half-open: a region owns `[minX, maxX)`, so a point exactly on a shared edge
belongs to precisely one server, never both and never neither. That rule is invisible unless
objects actually land on the line — which in every other scenario they essentially never do.

```powershell
-Workload seam -Servers 4
```

**Use it for:** proving the ownership rule is exact. Without it, the boundary check is never
really tested.

---

## `injection`

The world starts **empty**. Objects appear over time at a fixed rate — 160 per second — split
evenly between a site near a boundary and a site at each region's centre.

This reproduces the benchmark published by **Aura Projection**, the earlier distributed physics
system this project is compared against. It is the only scenario where the world grows during
the run, which stresses everything that assumes a fixed population.

```powershell
-Workload injection -Objects 0 -Ticks 7200 -HaloWidth 8 -HaloLookahead 4 -HaloReliable
```

Note `-Objects 0`: the count is ignored because nothing is created at the start.

**Use it for:** the comparison against published results, and as a stress test — 160 per second
for 60 seconds is about 9,600 objects.

> The rate is 160/second **in total**, not per server. Adding servers divides the same work
> rather than adding more of it, which is what makes it a scalability test.

See [the AP benchmark result](superpowers/results/2026-08-21-AP-injection.md) for the measured
numbers and the differences from the original.

---

## Cross-boundary collision (the "halo")

By default, objects near a boundary **do not collide with objects on the other side** — each
server only knows about its own objects. Turning this on makes each server publish the objects
near its edges to its neighbours, which hold read-only copies that can be collided with but are
never simulated.

```powershell
-HaloWidth 8 -HaloLookahead 4 -HaloReliable
```

| Option | Meaning |
|---|---|
| `-HaloWidth W` | How wide the shared band is. `0` turns the feature off. |
| `-HaloLookahead N` | How many steps ahead a shared object is scheduled, to absorb network delay. Default 4. |
| `-HaloReliable` | Send the updates reliably. Not needed in production; **needed for a repeatable run**, because otherwise which updates get dropped differs every time. |

**Choosing the width.** Too narrow and objects cross the band between updates and miss each
other. The server calculates the minimum safe width for your settings and warns loudly if you go
below it — do not ignore that warning, it means contacts are being missed silently.

---

## Choosing a workload: worked examples

**"Is 2 servers faster than 1?"**
`uniform`, sweep `servers`, use `-Seconds` with repeats.

**"Does the split lose objects?"**
`uniform` or `seam`, use `-Ticks` for repeatability, check `analyse.py` exits zero.

**"Does cross-boundary collision work?"**
`headon` with `-HaloWidth 8`. Compare against `-HaloWidth 0`, which should miss every contact.

**"Does load balancing help?"**
`cluster`, compare `-RebalanceInterval 0` against `-RebalanceInterval 400`.

**"How does it compare to published work?"**
`injection`. See [Running experiments](RUNNING-EXPERIMENTS.md).
