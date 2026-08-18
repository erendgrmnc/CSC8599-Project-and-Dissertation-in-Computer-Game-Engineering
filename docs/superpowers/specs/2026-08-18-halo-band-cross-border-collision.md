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

The last one is the subtle one. `ImpulseResolveCollision` writes velocity to **both** bodies. The
shadow's position will not drift, because it is not integrated — but its *velocity* will be
corrupted by every contact it takes part in, and velocity feeds the next contact's relative-velocity
term. So the shadow's state must be restored from the last received update at the top of every tick,
not merely on arrival.

---

## 3. Determinism

The region-local increment ends with both servers bit-identical for 7,200 ticks. That must survive.

Halo updates arrive over the network, so their arrival tick is not deterministic — exactly the
problem handoff had. The fix is the same one: **apply a halo update at `senderTick + lookahead`**,
not on arrival, reusing the `--handoff-lookahead` mechanism and the same "too late" counter
(`haloLate`, alongside `hoLate`). A halo update that misses its window is dropped and counted, never
applied early.

This makes the halo band and the lookahead interdependent: `H` must be wide enough that an object
cannot cross from outside the band to a contact in fewer than `lookahead` ticks. With `v_max` the
maximum object speed and `dt` the substep,

```
H  >=  v_max * lookahead * dt  +  2 * r_max
```

Below that the band is a correctness bug, not a tuning parameter, and it will present as
occasional missed contacts that vary with load. The band width must therefore be **derived and
asserted**, not chosen by eye. `--halo-width` overrides it for experiments, with a loud warning
when the override is below the derived floor.

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
- the pair's `a`/`b` orientation (already ordered by world ID — this is why that fix mattered),
- the contact point and normal, which come from positions that must be identical, not merely close,
- the elasticity and friction coefficients, which come from the archetype.

---

## 5. Increments

### B0 — Measure the "before" *(done)*

`contacts` per tick in the CSV and cumulatively in `@@FINAL`, plus `--workload headon`. §0 is its
output. Shipped as `7c3ece0`.

### B1 — Shadow objects exist, and are inert

Add a shadow flag to `GameObject` distinct from `SetActive`, and a `mHaloObjects` map on
`ServerWorldManager`. Shadows are constructed from the archetype exactly as handoff arrivals are
(A3 already provides `CreateObjectFromArchetype`), added to the `GameWorld`, and excluded from
integration, handoff, snapshots and command targeting. **No halo traffic yet** — the set stays
empty, so every existing measurement must come out unchanged. That is the point of doing it
separately: it is the increment that can break everything and produce no new behaviour to explain it.

### B2 — Publish the band

Each server, once per update, sends every neighbour the objects it owns that lie within `H` of the
border it shares with that neighbour. One packet type, `HaloUpdatePacket`, carrying a fixed-size
batch of `(objectID, archetypeID, position, velocity, orientation, angularVelocity, senderTick)`.
Directed, not broadcast — `SendPacketToServer`, as handoff now is.

Band membership is computed from the same `RegionOwnership.h` bounds the ownership rule uses. It
must not be a second, separately-written border test; that is the bug the ownership unification
fixed.

### B3 — Apply on a deterministic tick

Receiver queues updates and applies them at `senderTick + lookahead`, mirroring
`FlushScheduledHandoffs`, including its `(applyAtTick, objectID)` sort. Adds `haloLate`.

### B4 — Shadows collide

Include shadows in `mDynamicObjectList` so `BroadPhase` forms pairs with them. Re-impose shadow
state at the top of each tick, before `IntegrateAccel`. This is the increment where `headon` should
go from 100 handoffs and no contacts to 0 handoffs and 50 contacts.

### B5 — Retire a shadow

An object that leaves the band, or is handed off, or is destroyed, must stop being shadowed.
A shadow with no update for more than `lookahead` ticks is stale and is torn down — the same
teardown path A4 introduced. Without this the halo grows monotonically and I6 comes back through
the side door.

### B6 — Handoff becomes a promotion

An object crossing the border is, by construction, already a shadow on the receiving server. The
handoff can then stop shipping full state and become "the object you are already tracking is now
yours", which removes the ownership gap §0.7 describes: there is no window in which no server has
the object, because the receiver had a copy before the transfer began.

This is the increment that makes the halo pay for itself, and it is deliberately last: it changes
handoff, which every existing invariant is measured against.

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
