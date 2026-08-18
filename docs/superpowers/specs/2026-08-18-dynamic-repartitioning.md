# Dynamic repartitioning (2026-08-18)

Follows the halo band increment. That one ended by demonstrating something this one has to fix: once
servers exchange state every tick, a partition that lets one server fall behind is a **correctness**
problem, not only a slow one.

---

## 0. The problem, verified

Region borders are computed once, by `GameInstance::CalculateServerBorders`, before any object
exists. They never move. The `shuttle` workload launches every object from one start offset, so a
static partition begins badly loaded and stays that way.

400 objects, 3,600 paced ticks, seed 42, `--halo-width 8 --halo-reliable`:

| | 2 servers | 4 servers |
|---|---|---|
| objects per server | 359 / 41 | 33 / 3 / **326** / 38 |
| busiest server's share | 90% | **81%** |
| wall clock for the same 3,600 ticks | 70.4 s vs 30.2 s | 102.5 s vs 30.4 s |
| slowdown of the busiest server | **2.33x** | **3.37x** |
| `haloLate` | 10,376 | **65,307** |
| `hoLate` | 0 | 2 |

Three separate things go wrong, and only the first is the one usually discussed.

1. **Throughput.** The busiest server takes 2.3x (2 servers) or 3.4x (4 servers) as long to simulate
   the same interval. Adding servers made the *worst* case worse, because a finer partition gave the
   loaded region a smaller slice without moving any objects out of it.

2. **The halo stops working.** A halo update is applied at `senderTick + lookahead`. A server that
   cannot hold pace falls behind its peers in real time, so its samples arrive stamped with tick
   numbers far below the receiver's counter and miss their slot: 65,307 late updates on a 4-server
   run. Late updates are applied anyway rather than dropped, so the simulation continues, but it is
   no longer reproducible and the shadows are wrong.

3. **Handoff determinism starts to go too.** `hoLate = 2` on the 4-server run. The same skew that
   breaks the halo will eventually break the mechanism the whole reproducibility apparatus rests on.

There is also a cost signal worth recording: on the 4-server run server 0 held **51 shadows against
33 owned objects**. A bad partition does not just overload one server, it makes the halo expensive
everywhere else.

---

## 1. What this increment is, and is not

**Is:** moving the borders while the simulation runs, without losing an object, without two servers
owning one, and without breaking reproducibility.

**Is not:** a good load-balancing policy. The policy is deliberately the *last* increment and the
simplest thing that works, because the interesting and risky part is the mechanism. A perfect policy
on an unsafe mechanism is worthless; a crude policy on a safe one is a result.

---

## 2. Why this is now a small change

Everything expensive was paid for by the previous two increments.

| Needed | Already there |
|---|---|
| A server holds only its own region | region-local world state (A4/A5) |
| An object can be built on arrival from nothing | construct-on-arrival (A3) |
| Ownership changes atomically at an agreed tick | scheduled release (B6) |
| Commands still reach a moving object | forwarding table + client-stamped position |
| One ownership rule, in one place | `RegionOwnership.h` |

Under the pre-seed model a border move meant mass *reactivation* across servers that already held
everything — the cost was hidden, and the result would not have generalised to a world too large to
pre-seed. It is now a bulk handoff, which is the honest cost and the thing worth measuring.

The remaining work is: agree on new borders, adopt them on the same tick everywhere, and let the
existing border check do the rest.

---

## 3. The mechanism

### 3.1 Adoption must be atomic

Every server must switch to the new partition on the **same simulated tick**. If two servers
disagree about where a border is, even for one tick, `OwningServerFor` gives different answers on
each and an object is either owned twice or not at all.

So a repartition carries an **effective tick**, exactly as a handoff carries `senderTick + lookahead`
and a halo update carries its sample tick. The manager picks it far enough ahead that every server
has received the packet before it arrives. A server that receives one whose effective tick has
already passed adopts immediately and counts it (`repartLate`), because the alternative — ignoring
it — leaves that server on a partition nobody else is using, which is unrecoverable.

### 3.2 Borders are mutated in place, not replaced

`ServerWorldManager` holds `PhysicsServerBorderData&` and a `map<int, PhysicsServerBorderData*>`
handed to it at construction. Swapping the pointers would dangle every reference taken from them, so
adoption overwrites the **contents** of the existing structs.

> `TestObject` also takes a `PhysicsServerBorderData&` and stores a **copy** of it. That copy is
> written in the constructor and never read anywhere, so it cannot go stale — but it is exactly the
> kind of thing that would silently become wrong, and it should be deleted rather than left.

### 3.3 The bulk handoff falls out

Once the borders change, `CheckPositionOutOfServerBoundaries` sees every object that is now outside
its owner's region and hands it off through the ordinary path. With B6 the sender holds each object
until the receiver installs it, so ownership stays continuous through the move.

The only new thing is *volume*: a border move can transfer hundreds of objects in one tick, where
normal traffic is a handful. Two consequences to watch:

- one `StartSimulatingObjectPacket` per object, reliably, in a single tick;
- the receiving server constructs all of them, mostly via construct-on-arrival, since only the ones
  in the halo band are already shadowed.

Both are measured rather than assumed (§6).

### 3.4 Determinism

A repartition decision that depends on when the manager happened to sample is not reproducible. The
decision must be a pure function of inputs that are themselves deterministic:

- servers report `(tick, ownedCount)`;
- the manager decides using the reports **for one specific tick**, not "the latest";
- if a report for that tick is missing, the round is skipped — deterministically, by the same rule
  on every run.

Until the policy increment lands, `--repartition-at TICK:BORDERS` forces a known repartition at a
known tick, which is what makes the mechanism testable independently of any policy.

---

## 4. Invariants

I1–I8 continue to hold. Repartitioning stresses two of them specifically, and adds one.

- **I1, continuously.** The per-tick ownership check added with B6 is the acceptance test here:
  summing `owned_objects` across servers must never dip below the world total (an object owned by
  nobody) nor rise above it (owned by two). A border move is the single most likely way to break it.
- **I2 conservation** across the move.
- **I9 (partition agreement).** Every server must be using the same partition on any given tick. Not
  directly observable from one server, so it is checked indirectly: a disagreement shows up
  immediately as an I1 violation, and `repartLate > 0` is the warning that it *could* have.

---

## 5. Increments

### C0 — Measure the "before"

§0 is its output, from the existing `@@FINAL` and per-tick metrics. No code needed: the imbalance was
already visible in `objs`, and the wall-clock figure is printed by the headless runner.

### C1 — Borders can move, at an agreed tick *(done)*

`DistributedRepartitionPacket` (a POD region array plus an absolute effective tick), broadcast
reliably by the manager, queued by each server and adopted when its tick counter reaches that tick.
Adoption overwrites the border structs in place. `--repartition-at TICK --repartition-x "x1,..."`
forces one. Shipped as `d44c981`.

#### Acceptance test

200 objects, `uniform`, 1,800 paced ticks, border moved from `x = 0` to `x = -75` at tick 900:

| | result |
|---|---|
| objects before / after | 105 / 95 — **48 / 152** |
| ticks with an object owned by nobody | **0** |
| ticks with an object owned by two servers | **0** |
| conservation delta | **0** |
| handoff parity (after subtracting in-flight) | **0** |

The per-tick total is exactly 200 on every tick through the move. Server 0 bulk-transferred 76
objects, and the new split (25% of the world) holds 24% of the objects, as it should.

#### The benefit, before any policy exists

400 objects, `shuttle` — the workload the static partition is worst on — 1,800 paced ticks, with the
border moved once at tick 300 from `x = 0` to a hand-picked `x = -40`:

| | static | repartitioned |
|---|---|---|
| objects | 359 / 41 | 100 / 300 |
| contacts | 974,047 / 108,139 (**9.0 : 1**) | 588,957 / 542,354 (**1.09 : 1**) |
| wall clock | **33.1 s** / 15.3 s | 22.3 s / **27.5 s** |
| busiest server | 33.1 s | **27.5 s**, 17% faster |
| slowest : fastest | 2.16 : 1 | **1.23 : 1** |

Only five sixths of the run was after the move, so a policy acting earlier would do better.

#### What this changes about C3

**Object count is the wrong thing to balance.** The winning partition above holds 100 objects against
300 — badly "unbalanced" by count — and yet its wall clock is nearly equal, because its *contacts*
are nearly equal. Contact count scales with local density, not with object count, and the halo adds
work to whichever server has the busier border.

So the policy should balance measured per-tick cost, or contacts as a proxy for it, not object
counts. That is a change of objective from what §5 originally proposed, and it is only visible
because C1 made it measurable.

#### Two things found on the way

**The region cache never noticed a border move.** `GetRegionBounds` caches the region list and
rebuilt it only when the border map's `size()` changed. That is fine while borders are fixed and
silently wrong once they can move: the first forced repartition changed the values, the cache kept
the old ones, and ownership answered from the old partition while the halo — which reads the map
directly — had already moved to the new one. The borders visibly changed and not one object was
handed off.

**Handoff parity needed redefining.** Since B6, `hoSent` counts the *start* of a transfer and
`hoRecv` its *completion*, so a run that ends mid-transfer is legitimately short. The object is not
lost — the sender still owns it, which conservation and the per-tick ownership check both confirm.
`@@FINAL` now reports `hoPending` and `analyse.py` subtracts it.

#### Reproducibility

A run containing a repartition reproduces its ownership exactly: object counts, pool sizes, handoff
counts and pending transfers are identical across a repeat pair. `contacts` differs by a few tens,
and `haloLate` by a few hundred, because the partition the test moves *to* is deliberately
unbalanced — which is §3.5 of the halo spec restated: once servers exchange state every tick, a
partition that lets one fall behind loses reproducibility. A rebalancing move should therefore
*improve* reproducibility rather than cost it, and confirming that is part of C4.

### C2 — Servers report load

`(tick, ownedObjects)` to the manager, at a fixed tick interval rather than a wall-clock one, so the
reports are themselves deterministic.

### C3 — A policy

The simplest defensible one: equalise object counts along the axis the partition already splits, with
hysteresis so a border does not oscillate around a threshold. Deliberately crude; the mechanism is
what matters.

### C4 — Evaluate

Busiest-server tick cost and wall clock, static vs dynamic, on `shuttle` — the workload the static
partition is worst on. Plus the cost of the moves themselves.

---

## 6. Verification

1. **Ownership continuity** through a forced move, via `analyse.py`'s per-tick check. This is the
   acceptance test: 0 gap ticks and 0 double-owned ticks across a run containing a repartition.
2. **Conservation** across the move.
3. **Reproducibility** of a run containing a forced repartition — same seed, same effective tick,
   byte-identical.
4. **The move's cost**: how many objects transfer, and what the tick containing the move costs
   relative to its neighbours. A move that stalls a server for a visible interval is a result, not a
   failure, but it has to be reported.
5. **`headon` unchanged**, since it should never trigger or need a repartition.

---

## 7. Risks, ranked

| Risk | Severity | Mitigation |
|---|---|---|
| Servers adopt on different ticks | **Critical**, silent | Effective tick, and I1 checked per tick rather than at the end |
| Bulk handoff drops objects | High | Reliable transfer; conservation checked across the move |
| The move's tick cost is a spike | Medium | Measured, and reported rather than smoothed away |
| Border oscillation | Medium | Hysteresis in C3; a border that moves every round costs more than the imbalance it fixes |
| A repartition during a handoff | Medium | The object is owned by the sender until the agreed tick, so it is repartitioned as the sender's; the pending-release guard already stops it being re-sent |
| Region becomes empty | Low | Legal, and the partition must be allowed to produce it rather than special-casing |

---

## 8. Explicitly out of scope

- **Non-rectangular partitions.** Borders stay axis-aligned rectangles. A quadtree or KD partition
  would fit real object distributions better and is the obvious follow-on, but it changes
  `RegionOwnership.h`, which every role depends on.
- **Migrating a server.** The set of servers is fixed for an instance; only the borders move.
- **Interest management.** Still unaddressed, and now the largest remaining source of per-server cost
  that scales with world size rather than region occupancy.
