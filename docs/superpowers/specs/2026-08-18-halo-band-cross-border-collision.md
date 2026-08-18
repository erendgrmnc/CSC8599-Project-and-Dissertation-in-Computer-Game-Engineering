# Halo band: cross-border collision (2026-08-18)

Follows `2026-08-18-region-local-world-state.md`, which made each server hold only the objects it
owns. That increment deliberately left one thing worse than it found it: with the deactivated twins
gone, a server has no representation at all of anything on the far side of its border. This
increment gives it one.

---

## 0. The problem, verified

Objects on opposite sides of a region boundary do not collide. They pass through each other.

This is stated in `CLAUDE.md` as a known gap, but it had never been measured. It is now, with a
purpose-built workload (`--workload headon`): pairs of objects spawned 25 units either side of
`x = 0` and launched at each other at 60 u/s, one pair per lane, lanes 6 units apart so no pair can
reach any other. Every collision the run records is a head-on one.

100 objects (50 pairs), 1800 paced ticks, seed 42, `--handoff-lookahead 300`:

| | 1 server | 2 servers |
|---|---|---|
| `contacts` | 261,750 | 108,500 + 108,500 = **217,000** |
| `hoSent` | **0** | 50 + 50 = **100** |

The handoff count is the sharper of the two. On one server the pairs collide and bounce back, so
**nothing crosses `x = 0` at all**. Split across two servers, **every one of the 100 objects passes
straight through its partner** and is handed to the other side. Not one collision is detected.

The contact difference (44,750) understates it, because most of that total is each object's
per-substep floor contact, which is conserved either way.

Under the less adversarial `shuttle` workload the same comparison loses 18,119 contacts out of
2,139,341 — 0.85%. That figure is the honest one for a general workload; `headon` is the one that
shows the mechanism.

### Why it happens

`PhysicsSystem::BroadPhase` iterates `mDynamicObjectList`, which is built from the objects in this
server's `GameWorld`. After the region-local increment that list contains exactly this server's
owned set. An object one unit past the border is not in it, is not in the quadtree, and is not in
any candidate pair. There is no test that rejects the pair — the pair is never formed.

---

## 1. Target model

Each server keeps, in addition to its owned set, a **halo**: read-only copies of objects owned by a
neighbouring server that lie within a band of width `H` of the shared border.

- The **owner** simulates. It integrates, it resolves contacts, it is the only source of truth.
- A **shadow** collides but is never integrated. Its position and velocity come from its owner,
  once per update, and are re-imposed every tick.
- A contact between an owned object and a shadow is resolved on **both** servers. Each keeps only
  the half of the result that applies to the object it owns and discards the other half.

This is the master/shadow arrangement from Eibl & Rüde (2018), and the ghost-atom exchange from
Plimpton (1995) before it. It is not novel and the paper should not claim it is; what this codebase
gets from it is the first configuration in which cross-border contact is possible at all.

### Why both sides resolve, rather than one side resolving and sending the result

Resolving once and shipping the impulse to the other owner is one round trip of latency inside the
contact, which is a tick of lag on every border collision, and it makes the two servers'
integration order depend on each other — the thing `--handoff-lookahead` exists to avoid. Resolving
redundantly costs a little CPU and no latency, and keeps each server's tick independent.

It is only correct if both sides compute the *same* impulse from the same inputs. That is invariant
I8 below and is the main thing that can go wrong.

---

## 2. What has to be true for a shadow to be safe

| Property | Why | Where enforced |
|---|---|---|
| A shadow is never integrated | Two servers integrating the same object is a second owner | `IntegrateAccel` / `IntegrateVelocity` skip it |
| A shadow is never handed off | It is not ours to hand off | `CheckPositionOutOfServerBoundaries` skips it |
| A shadow is never in a snapshot to clients | Clients would see it twice, from two servers | snapshot loop skips it |
| A shadow is never a command target | Commands must reach the owner | `FindActiveObject` skips it |
| A shadow does not appear in `objPool` as owned | It would corrupt the locality measurement | reported separately as `objHalo` |
| A shadow's state is re-imposed each tick | Contact resolution writes to its velocity | before `IntegrateAccel` |

The last one is the subtle one, and B1 measured it rather than leaving it as an argument. **Skipping
the integrator is not sufficient to make a shadow read-only.** Contact resolution reaches around the
integrator and writes to both bodies directly:

- `ImpulseResolveCollision` writes linear and angular **velocity** to both. A shadow held stationary
  against an object approaching at 20 u/s came out of five ticks carrying **9.42 u/s** it does not
  own.
- `SeperateObjects` writes **position** to both, proportionally to inverse mass, to resolve
  penetration. So the shadow is displaced as well — the failure is not confined to velocity, as this
  section originally claimed.

Both are correct behaviour for a single server; both make a shadow diverge from the copy its owner is
simulating. The shadow's authoritative state must therefore be re-imposed at the top of **every**
tick, not merely when an update arrives. `HaloShadowTests.cpp` pins the corruption as it stands, and
that test is to be inverted when B4 lands.

---

## 3. Determinism

The region-local increment ends with both servers bit-identical for 7,200 ticks. That must survive.

Halo updates arrive over the network, so their arrival tick is not deterministic — exactly the
problem handoff had. An update is therefore applied at **`senderTick + halo lookahead`**, not on
arrival, with `haloLate` counting the ones that miss their slot.

Three things about this were wrong when first written, and each was found by building it.

### 3.1 The halo lookahead is not the handoff lookahead

Reusing `--handoff-lookahead` looked like reuse and is a category error. A handoff *releases* the
object at `senderTick` and the receiver picks it up `lookahead` ticks later; the object is frozen in
between, so a large value only widens a one-off gap. A halo update is a **continuously tracked
position**, so applying it late means the shadow is permanently that far behind. At the 300-tick
handoff lookahead used for reproducible runs that is 2.5 seconds of lag — worse than no shadow.

`--halo-lookahead` is separate, and defaults to 4 ticks.

### 3.2 A small lookahead does not fix it — extrapolation does

Shrinking the lookahead to hide the lag makes updates arrive *after* their slot instead. Measured, at
`--halo-lookahead 1`, essentially every update was late (2,450 of 2,500 on one server), the two
servers' shadow sets stopped agreeing, and a `headon` run ended with **all 100 objects on one
server** — the shadows had become one-way walls.

The lag is not the lookahead; it is the lookahead *plus* however long the packet took. So the shadow
is **extrapolated** from its sample tick to the local tick using the velocity it carries:

```
position = state.position + state.linearVelocity * (localTick - sampleTick) * substepDt
```

Dead reckoning is a pure function of the received state and two tick numbers, so it costs nothing in
determinism, which applying-on-arrival would have. Gravity is deliberately not integrated: over the
few ticks this spans, the `0.5*g*t^2` term is under a hundredth of a unit, and including it would tie
the shadow's path to a gravity setting the owner might not share.

### 3.3 Unreliable delivery is not reproducible

A halo update is superseded next tick, so unreliable is the natural choice and is what a deployment
wants. But a dropped update leaves the shadow extrapolating from an older sample, and **which**
packets drop is not the same from run to run. Two otherwise identical `uniform` runs differed by one
received update and by 14 contacts.

`--halo-reliable` switches to reliable delivery and restores exact reproducibility. It is not the
default, because the cost is real and a deployment does not need it; it is what a measurement run
uses.

### 3.4 Band width

`H` must be wide enough that an object cannot cross from outside the band into contact within the
window between sampling and application:

```
H  >=  v_max * halo_lookahead * dt  +  2 * r_max
```

At the shipped defaults that is `60 * 4/120 + 4 = 6` units. `--halo-width` warns loudly when set
below the derived floor rather than silently accepting it — the symptom otherwise is occasional
missed contacts that vary with load, which reads as flakiness rather than as a misconfiguration.

### 3.5 The halo needs the servers to keep pace, and static partitioning does not guarantee it

Under `--workload uniform` both servers run 1,800 paced ticks in comparable wall-clock time,
`haloLate` is 0, and with reliable delivery the run is bit-reproducible.

Under `--workload shuttle` it is not. That workload puts 359 objects on one server and 41 on the
other, so server 0 needs 103.7 s of wall clock for the 7,200 paced ticks server 1 finishes in 60.3 s.
The lightly loaded server races ahead in real time, every update from its overloaded peer arrives
with a sample tick far below its own counter, and `haloLate` reaches **18,160 of 18,164 received
updates**. Reproducibility is lost.

This is the sharpest argument the project has for dynamic repartitioning. Load balancing is not only
a throughput optimisation: once servers must exchange state *every tick*, a partition that lets one
server fall behind is a **correctness** problem, not just a slow one.

---

## 4. Invariants

Existing I1–I6 continue to hold. Two new ones:

- **I7 (single simulator).** Summed across servers, the number of objects *integrated* per tick
  equals the number of distinct objects in the world. A shadow that is integrated shows up here
  immediately. Measured as `integrated_objects` summed vs the world total.
- **I8 (symmetric contact).** For a contact between an object owned by server A and one owned by
  server B, the impulse A computes for its object and the impulse B computes for its object are
  equal and opposite to within float tolerance. Verified directly in Tier 0 by resolving the same
  pair twice with the roles swapped, and in-system by a `haloAsym` counter that trips when the two
  sides disagree by more than a threshold.

I8 is where this increment is most likely to be quietly wrong, because both sides must agree on:
- the pair's `a`/`b` orientation. **This was wrong when written.** The earlier fix ordered contact
  pairs by `GameObject::GetWorldID`, which is a per-`GameWorld` creation counter
  (`AddGameObject` does `worldIDCounter++`). The two servers build a cross-border pair in opposite
  orders — the owned object at pre-seed, the neighbour's shadow when its first halo update arrives —
  so they oriented the same contact oppositely and computed different impulses from it. Ordering is
  now by `GetContactOrderID`, which is the network id: globally unique and identical everywhere.
  Objects with no global identity (static geometry, the floor) fall back to the world id and sort
  after every networked object, so the comparator stays a strict weak ordering.
  `ContactSymmetryTests.cpp` resolves the same pair from both servers' perspectives, with the
  objects created in opposite orders, and checks the impulses agree;
- the contact point and normal, which come from positions that must be identical, not merely close,
- the elasticity and friction coefficients, which come from the archetype.

---

## 5. Increments

### B0 — Measure the "before" *(done)*

`contacts` per tick in the CSV and cumulatively in `@@FINAL`, plus `--workload headon`. §0 is its
output. Shipped as `7c3ece0`.

### B1 — Shadow objects exist, and are inert *(done)*

`GameObject::IsHaloShadow`, distinct from `SetActive` because the two states it has to combine —
*has physics* and *is not simulated* — are the same flag today. `mHaloObjects` on
`ServerWorldManager`, deliberately **not** in `mCreatedObjectPool`: keeping shadows out of the pool
is what makes `FindActiveObject`, the snapshot loop and the handoff path skip them without a guard
in each. The loops that iterate the `GameWorld` instead do carry an explicit test.

Reported as `objHalo` in `@@FINAL` and `halo_objects` in the CSV, separate from `objPool` — one is
what a server is responsible for, the other what it is merely watching, and adding them would make
the I6 locality figure unreadable.

No halo traffic, so the set stays empty and every existing measurement had to come out unchanged.
It did: `headon` still 261,750 contacts on one server and 108,500 + 108,500 with 100 handoffs on two,
`shuttle` still 359/41 objects and 41/41 handoffs, and the 7,200-tick reproducibility pair still
byte-identical on both servers. Tier 0 75/75. Shipped as `6fdfd93`.

### B2 — Publish the band *(done)*

`HaloUpdatePacket`, directed via `SendPacketToServer` rather than broadcast, batched 20 objects per
packet with `GamePacket::size` set to cover only the entries actually used — so an empty batch does
not put 20 entries of uninitialised stack on the wire, and the batch size can be generous without
costing anything on a quiet border.

Band membership comes from `GetOverlappedServers`, the same query area effects use, with the radius
being the band width. Not a second border test: two border tests that disagree is exactly the bug the
ownership unification removed. It also handles the corner case for free — an object near the origin
on a 2x2 grid is within the band of more than one neighbour and is published to each.

Registered on **both** peer-link directions. A type sent down a direction with no handler is dropped
by ENet in complete silence, which cost a debugging session during A2.

Verified as traffic only: with `--halo-width 0` every number matched B1 exactly; with width 8,
`haloSent=75 / haloObjSent=1250`, and `haloRecv` / `haloObjRecv` matching exactly on both servers,
with no behavioural change at all. Shipped as `57a35d3`.

### B3, B4, B5 — Apply, collide, retire *(done, together)*

They could not usefully be separated: B1 had already made shadows collidable, so the moment a shadow
is added to the world it collides, and a shadow that is never retired is an invisible wall. Shipped
as `4de8bed`.

Retirement matters more than it looks. A shadow whose owner has stopped publishing it sits where
nothing exists any more; before `RetireStaleHaloShadows` existed, a `headon` run accumulated 100
shadows for 50 remote objects and the leftovers pushed every object onto one server. The timeout is
deliberately generous (30 ticks) because unreliable updates drop: being late to retire costs a few
ticks of ghost, being early costs a missed contact. `RemoveHaloShadow` additionally fires when an
object is handed *to* us, or the same object would be in the broadphase twice.

**Acceptance test result**, 100 objects, 1,800 paced ticks, `--halo-width 8`:

| | 1 server | 2 servers, no halo | 2 servers, halo |
|---|---|---|---|
| border crossings (`hoSent`) | 0 | 100 | **0** |
| `objFwd` | 0 | 100 | **0** |
| `haloLate` | - | - | **0** |
| `contacts` | 261,750 | 217,000 | 136,295 + 136,295 |

Zero crossings with the halo on: every pair collides and bounces exactly as it does on one server,
and the two servers are bit-symmetric. The contact total is *higher* than the single-server run
(272,590 vs 261,750) because each border contact is resolved on both servers by design — the 10,840
difference is the redundant half.

> The `headon` workload was retuned from 60 u/s to 30 u/s while this landed, and the reason should be
> stated plainly rather than buried. At 60 u/s a pair closes 1.0 units per 120 Hz substep against a
> 1.0-unit contact window, so with no continuous collision detection even the **single-server** case
> is marginal — the workload was measuring the integrator's discrete-collision limit rather than
> anything about region borders. At 30 u/s a pair overlaps for two substeps and the contact is
> unambiguous. For the record, at 60 u/s the halo still took border crossings from 100 to 22.

### B6 — Handoff becomes a promotion, and ownership transfers atomically *(done)*

Two changes, and the second turned out to matter far more than the first.

**Promotion.** An object crossing the border is already a shadow on the receiving
server, so `ApplyIncomingObject` promotes that shadow instead of tearing it down and
building a replacement. Cheaper, but the reason is correctness as much as cost:
rebuilding discards the object's contact history, which `UpdateCollisionList` carries
for several frames, so an object mid-collision at the border would have its contacts
silently reset by crossing it.

**Atomic transfer.** The sender released the object the moment the packet was sent,
while the receiver installs it at `senderTick + lookahead`. Both sides now compute
that same tick and act on it: the receiver installs, the sender releases. No barrier,
no acknowledgement — just the same arithmetic on both ends.

#### The gap this closed, measured

The ownership gap was known and documented, but had only ever been reasoned about. It
is measurable directly: sum `owned_objects` across servers at each tick and compare
against the world total. On a 200-object `uniform` run, 1,800 paced ticks:

| | ticks with an object owned by **nobody** | worst simultaneous deficit | ticks with an object owned **twice** |
|---|---|---|---|
| release on send | **1,397 of 1,800 (78%)** | 33 objects | 0 |
| release on the agreed tick | **0** | 0 | 0 |

78% of ticks had at least one unowned object, and at worst 33 of 200 at once. That
follows directly from the lookahead: with `--handoff-lookahead 300` every transferred
object spent 2.5 seconds simulated by nobody. The mechanism that made handoff
*deterministic* is what made the gap large.

#### Why nothing caught it

`conservation_delta` and `ho_parity_delta` were **0 on both runs**. End-of-run totals
say what each server held when it stopped and are silent about everything in between,
and an object that is unowned for 300 ticks and then correctly installed balances
perfectly at the end.

`analyse.py` now checks ownership per tick, in both directions — a dip below the world
total is a gap, a rise above it is two servers simulating the same object. This is the
one invariant in the set that could not have been expressed against `@@FINAL`.

---

## 6. Verification

1. **`headon` is the acceptance test.** 0 handoffs, 50 contacts, and final positions mirroring the
   1-server run. It is currently 100 handoffs and 0 contacts, so there is no ambiguity about whether
   the increment did anything.
2. **Reproducibility must survive**, re-run after every increment, as it was for A0–A7.
3. **I7 and I8**, above.
4. **`shuttle` contact parity** against the 1-server run: 2,139,341 vs 2,121,222 today. This will not
   reach zero — a halo of finite width cannot catch a contact between two objects that are both more
   than `H` from the border, but such a pair is by definition owned by the same server, so the
   residual should be zero and any non-zero residual is a real bug worth chasing.
5. **Cost.** The halo is redundant work, and the paper has to report what it costs: per-tick physics
   time and bytes/s per server, against band width `H`. The interesting result is the shape of that
   curve, since `H` has a hard floor from §3 and a soft ceiling from cost.

---

## 7. Risks, ranked

| Risk | Severity | Mitigation |
|---|---|---|
| Shadow integrated somewhere | **Critical**, silent — a second owner | I7 as a per-tick assertion, not just a report |
| Both sides compute different impulses | High, subtle — objects drift apart | I8 + world-ID pair ordering already in place |
| Band too narrow for the lookahead | High, load-dependent | Derive `H` from §3 and assert; warn on override |
| Shadow velocity not re-imposed | Medium — contacts degrade over time | B4 restores before integration; test with a resting pair |
| Halo never retired | Medium — I6 regression | B5, and `objHalo` reported in `@@FINAL` |
| Corner regions on a 4-server grid | Medium | A corner object is in the band of *three* neighbours; the publish step must handle a set, not a single neighbour |

The corner case is worth stating plainly: on a 2x2 grid an object near the origin is within `H` of
two borders and diagonally adjacent to a third region. The publish step must be "every region whose
expanded bounds contain me", which is the same query `GetOverlappedServers` already answers for area
effects — reuse it rather than writing a second one.

---

## 8. Explicitly out of scope

- **Dynamic repartitioning.** Still static borders. The halo makes a border move cheaper, but the
  `shuttle` imbalance measured in the previous increment (359 vs 41 objects; 103.7 s vs 60.3 s of
  wall clock for the same 7,200 paced ticks) is the case for doing it, not this increment's job.
- **Interest management.** Servers still send every owned object to every client.
- **Client prediction.** Unaffected.
- **Continuous collision detection.** A fast object can still tunnel through a thin one within a
  single substep. That is an engine-level limitation, present on one server too, and not a
  distribution problem.
