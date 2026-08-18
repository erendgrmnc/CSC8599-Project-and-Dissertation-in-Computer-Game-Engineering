# Region-local world state (2026-08-18)

Design for removing the pre-seed-everything model, so that a server's state cost is
`O(its region)` rather than `O(the whole world)`.

This is increment 1 of the giant-world track. It deliberately does **not** add cross-border
collision — that is the halo increment, and it is much easier to build on top of this one than
alongside it. See §8.

---

## 0. The problem, verified

`ServerWorldManager::CreatePlayerObjects` loops over every player and calls `CreateObjectGrid`
(`ServerWorldManager.cpp:905`), which instantiates **every object in the world on every server**:

```cpp
// ServerWorldManager.cpp:940-946
if (IsObjectInBorder(transform.GetPosition())) {
    mTestObjects.push_back(dynamic_cast<TestObject*>(obj));
}
else {
    obj->SetActive(false);      // <-- still allocated, still in the world
}
mGameWorld->AddGameObject(obj);
```

An object outside the server's own region is fully constructed — `GameObject`, `PhysicsObject`,
`Transform`, `NetworkObject`, collision volume — added to `mGameWorld`, added to
`mCreatedObjectPool`, and merely flagged inactive. `CreateReplicatedSpawn`
(`ServerWorldManager.cpp:255`) reproduces the same model for runtime spawns, by design:

> "Deactivated: this server holds the twin so a future handoff can reactivate it, exactly as it
> would for a pre-seeded object it does not currently own."

**Consequence.** Per-server memory and construction cost scale with the total world, not with the
region. World size is therefore capped by what a *single* machine can hold, which is precisely the
thing distributing the simulation is supposed to remove. At the current 400-object measurement
worlds this is invisible; it is the hard ceiling on the giant-world goal.

Note this is *not* a bug — it is a deliberate simplification that makes handoff trivial (the
receiver already has the object, so handoff is "reactivate in place"). Removing it means paying
for that convenience elsewhere, which is what most of this document is about.

---

## 1. Target model

A server holds exactly:

| Set | Contents | State |
|---|---|---|
| **Owned** | objects whose position maps to this server's region under `OwningServerFor` | active, integrated, broadcast to clients |
| **Halo** *(later increment)* | objects within a band beyond the border, owned by a neighbour | read-only shadows, collidable, never integrated |
| **Forwarding** | `id -> last known owner` for objects that have passed through | 8 bytes per entry, no object |

Nothing else. An object this server has never owned and is not adjacent to does not exist here.

The halo row is stated now because it determines the shape of this increment: the replacement for
"deactivated twin" must not be something the halo work then has to undo. Specifically, the owned set
must be able to gain and lose objects at runtime without the rest of the system assuming a stable
pool — which is what construct-on-arrival and teardown-on-send give it.

---

## 2. What the deactivated twin is currently doing

This is the crux of the design. The twin is not one mechanism; it is silently serving five, and each
needs its own replacement. Missing any of them is how this change breaks something far from where it
was made.

### (a) Handoff target

`ApplyIncomingObject` (`ServerWorldManager.cpp:809`) looks the object up in `mCreatedObjectPool` and
**fails the handoff** if it is absent:

```cpp
auto poolEntry = mCreatedObjectPool.find(packet->objectID);
if (poolEntry == mCreatedObjectPool.end()) {
    ++mHandoffsFailed;
    return false;
}
```

*Replacement:* construct on arrival (§3, A3). Requires the packet to carry the archetype, which it
currently does not (§3, A1).

### (b) Ownership test

`FindActiveObject` (`ServerWorldManager.cpp:145`) returns `nullptr` for an inactive object, and its
comment states the assumption outright:

> "Every server holds a pool entry for every object; only the owner has it active. That is exactly
> the ownership test a command needs."

*Replacement:* absence from the pool becomes the ownership test. This is strictly simpler, and the
function body needs no change — the `IsNetworkActive()` check merely becomes redundant rather than
wrong. Keep it: with a halo, shadows will be present-but-inactive, and the check is then load-bearing
again.

### (c) Last-known-position for command routing

`TryGetLastKnownPosition` (`ServerWorldManager.cpp:158`) reads the twin's transform **without**
checking active state. A server that does not own an object can still say roughly where it is.

### (d) Relay-path coverage of race W2

This is the non-obvious one, and it is the reason the interaction design's "risky" load-bearing
handoff ack turned out to be unnecessary. From the increment-6 record: a server that has handed an
object away still holds its last known position, which lies in the *new* owner's region, so
`ApplyImpulse`-style commands arriving at the old owner resolve to the correct target and relay
there.

**Removing the twin removes that.** A server that has torn an object down has no position for it, so
the relay path loses its fallback and race W2 reopens. This must be replaced deliberately, not
discovered later as a regression in `cmdRelayed` accounting.

*Replacement:* a forwarding table (§3, A6).

### (e) Late-join manifest

`BuildOwnedObjectManifest` (`ServerWorldManager.cpp:349`) already filters to `IsNetworkActive()`, so
it is unaffected — it reports owned objects only. No change needed, but it must be re-verified,
because "owned" is currently derived from the active flag rather than from pool membership.

---

## 3. Increments

Each is independently buildable and independently verifiable. A1–A4 are preparation and can land
while the pre-seed model is still in place, which keeps every step testable against a working system;
A5 is the switch.

### A0 — Measure the "before"

Add per-server counters for pool size and owned-object count to `@@FINAL` and the per-tick CSV. This
is the baseline the whole increment is justified against, and without it the change is unfalsifiable.
Expected today: pool size identical on every server and equal to the world total; owned count equal
to region occupancy.

### A1 — Handoff carries the archetype

Append `int mArchetypeID` to `StartSimulatingObjectPacket`. **Append-only**, consistent with the
avatar and `mSenderTick` fields — every existing offset is unchanged, and the roles deploy
separately.

The sender reads it from `mObjectArchetypes`, which is already maintained for both pre-seeded and
runtime objects (`ServerWorldManager.cpp:934`, with a comment explaining that pre-seeded objects are
recorded precisely so a late joiner does not get the default shape).

### A2 — Handoff becomes directed

`SendTransactionHandshakePacket` currently uses `SendGlobalReliablePacket`
(`DistributedGameServerManager.cpp:592`), broadcasting the handoff to **every** peer. That is
harmless today because every peer has a twin and only the addressed one reactivates it. Once the
receiver *constructs* on arrival, a broadcast would have every server build the object.

Switch to `SendPacketToPeer` addressed at `newOwnerServerID`. The peer-link machinery already exists
and is used by the late-join manifest (`DistributedGameServerManager.cpp:863`).

> **Watch the indexing.** `StartDistributedGameServerPacket` carries two differently-indexed array
> families, and `connectedServerIDs[]` is what maps registration order back to real server ids. Using
> the array index instead mislabels every peer link — which is one concrete reason the transition ack
> never worked. Any new directed send inherits that hazard.

### A3 — Construct on arrival

`ApplyIncomingObject`: when the pool lookup misses, build the object from
`CreateObjectFromArchetype(packet->mArchetypeID, ...)` instead of counting a failure. The function
already does everything needed — creates the object, assigns the explicit network id, registers it
with `mPhysics`, and adds it to `mGameWorld` and both maps.

The tombstone and `mPendingDestroyOnArrival` guards must be checked **before** constructing, not
after, or a destroy that lost the race resurrects the object.

`mHandoffsFailed` keeps its meaning: a genuine failure is now an unknown *archetype* or a construction
error, not an unknown id. An unknown id becomes the normal case.

### A4 — Teardown on send

`HandleOutgoingObject` (`ServerWorldManager.cpp:890`) currently calls `SetActive(false)`. It becomes
`TeardownObject` plus a forwarding-table write, with the actual free deferred to
`FlushPendingDeletions` at the end of the tick — the existing rule, and it exists because
`UpdateCollisionList` dereferences raw `GameObject*` for several frames after a contact ends.

**Safety.** Handoff is sent reliably (`SendGlobalReliablePacket` today, `SendPacketToPeer` after A2),
so ENet guarantees delivery or reports the peer gone. Teardown-on-send is therefore safe against
ordinary loss in a way it would not be over an unreliable send. It is **not** safe against a peer
*disconnecting*, and the ownership gap in §0.7 of the interactions spec becomes permanent rather than
merely a window: today the sender still holds a deactivated copy, so the object is in principle
recoverable; after A4 it is not. Recording this as a known limitation is acceptable for now — a
handoff ack would close it, and A4 makes the case for finally implementing one, but it is not a
prerequisite.

### A5 — Region-local pre-seed

`CreateObjectGrid` runs its loop unchanged but instantiates only objects this server owns.

> **The loop must not be short-circuited.** Pre-seed network ids come from `mNetworkIdBuffer`, a
> monotonic counter incremented once per `AddNetworkObject` call in grid iteration order
> (`ServerWorldManager.cpp:658`). They are identical across servers *only* because every server walks
> the identical loop. Skipping iterations — rather than skipping instantiation within an iteration —
> shifts every subsequent id and silently desynchronises the entire id space. `objCounter` and the
> `DeterministicHash` call that picks cube-vs-sphere must advance for every grid cell regardless of
> ownership.

The cleanest form is to compute id, archetype and position for every cell, and gate only the
construction. That also gives A6 its data for free: a server can record the owner of every cell it
skips without building anything.

### A6 — Forwarding table

`std::map<int, int> mLastKnownOwner`, written when an object is handed away (A4) and when a spawn
broadcast is observed for an object owned elsewhere (A7).

`TryGetLastKnownPosition` gains a companion `TryGetLastKnownOwner`. Command routing prefers a live
object, falls back to the forwarding table, and only then reports `NotOwner` with
`correctedServerID = -1`, which the client resolves from its own `mObjectOwner` map — it already
maintains one from snapshots.

Cost is 8 bytes per entry and only for objects that have actually passed through this server, so it
does not reintroduce `O(world)`. If it ever needs bounding, entries are pure cache and can be dropped
safely — the client fallback covers a miss.

### A7 — Spawn becomes directed

`CreateReplicatedSpawn` currently builds a deactivated twin on every peer. Peers instead record a
forwarding entry. The owner constructs as it does now.

Note the existing comment on `CreateReplicatedSpawn` explains the twin exists so "a future handoff can
reactivate it" — that reason is retired by A3, and the comment must be updated rather than left to
mislead.

---

## 4. Invariants

All five existing invariants must still hold, unchanged:

| | |
|---|---|
| I1 | exactly one owner per object |
| I2 | object conservation |
| I3 | no resurrection |
| I4 | command accounting: `cmdApplied + cmdRejected + cmdDup` = client `cmdSent` |
| I5 | handoff parity: every send matched by exactly one receive |

I2 becomes materially harder to satisfy and materially more meaningful: today an object is
"conserved" partly because every server has a copy of it. After A5 the count is a real count.

One new invariant:

> **I6 — locality.** A server's pool size is bounded by its owned-object count plus its forwarding
> entries. Concretely: pool size must not scale with total world size at fixed region occupancy.

I6 is the whole point of the increment and is what A0's instrumentation exists to test.

---

## 5. Races to re-examine

The four handoff races (W1–W4) were analysed against the pre-seed model and must be re-checked, since
three of the four assumed a twin exists:

- **W2** (command arrives at the old owner after handoff) — was covered by the twin's stale position.
  Now covered by A6. **This is the one most likely to regress.**
- **W3** (destroy reaches the new owner before the object) — `mPendingDestroyOnArrival` still works,
  but A3 must consult it before constructing. Previously "structurally unreachable because every
  server holds a twin"; after A5 it becomes genuinely reachable, so the `--handoff-delay-ticks` fault
  injection is now the way to exercise it rather than a formality.
- **W4** (destroy and handoff cross) — unchanged in shape; verify the tombstone check ordering in A3.
- **W1** — re-derive; it was the least twin-dependent.

`--handoff-delay-ticks N` exists precisely to widen these windows deterministically and should be the
primary tool here.

---

## 6. Verification

1. **Tier 0** — `tools/InteractionTests` for construct-on-arrival, teardown-on-send, forwarding-table
   fallback, and the id-determinism property of the gated pre-seed loop. That last one is the highest
   value test in the set: it is the failure that would be silent and catastrophic.
2. **Reproducibility must survive.** The `--handoff-lookahead 300` configuration is now bit-identical
   across runs on both servers for all 7201 ticks (§18.6 of the interactions spec). That is a sharp
   regression detector and should be re-run after every increment.
3. **Invariants** — existing bounded runs with `@@FINAL` accounting; I4 and I5 exact.
4. **I6 / the actual claim** — the new experiment: hold objects-per-region fixed and grow the world by
   adding servers. Per-server pool size should stay flat where today it grows linearly. This is the
   plot that justifies the whole increment.
5. **Races** — W2/W3/W4 under `--handoff-delay-ticks`, which should now actually reach them.

---

## 7. Risks, ranked

| Risk | Severity | Mitigation |
|---|---|---|
| Pre-seed loop short-circuited, ids desync | **Critical**, silent | Gate construction, never iteration. Dedicated Tier 0 test comparing id sequences across simulated region assignments. |
| W2 relay regression | High, subtle | A6 lands with A4, not after. Compare `cmdRelayed` before/after. |
| Broadcast handoff not fully converted (A2) | High | Every server constructs a copy; I1 and I6 both break loudly. Caught by I6 instrumentation. |
| Peer disconnect loses an object permanently | Medium | Documented limitation; argues for the handoff ack but does not block. |
| Ownership gap widens | Medium | Already present; measured by `--handoff-delay-ticks`. |
| Reproducibility regression | Medium | §18.6 configuration re-run per increment. |

---

## 8. Explicitly out of scope

- **Cross-border collision / halo band.** Objects on opposite sides of a border still pass through
  each other after this increment. That is the next one, and this design is shaped so the halo becomes
  "a second set alongside owned" rather than a rework.
- **Dynamic repartitioning.** Borders remain static. This increment makes repartitioning *cheaper*
  (a border move becomes a bulk handoff rather than a global reactivation), but does not do it.
- **Interest management.** Servers still broadcast every owned object to every client. That is the
  client-side twin of this problem and is a separate increment.
- **Client prediction.** Unaffected.

---

## 9. Why this ordering

The halo, interest management and repartitioning increments all get simpler once a server no longer
pretends to know the whole world:

- The **halo** is a second, explicitly-scoped set. Under the pre-seed model there is no way to express
  "objects near my border" distinctly from "all objects I do not own", because they are the same set.
- **Repartitioning** moves a border, which under the pre-seed model means mass reactivation across
  servers that already hold everything — the cost is hidden and the result would not generalise to a
  world too large to pre-seed. Afterwards it is bulk handoff, which is the honest cost.
- **Interest management** is the same filtering question applied to the client link, and reuses the
  region/band vocabulary this increment establishes.

Doing any of them first would mean building against a model that is about to change.

---

## 10. Implementation notes (A0–A6 shipped)

A0–A7 are in. Every object a server holds is one it owns; no server builds anything for an object
owned elsewhere, at pre-seed or at runtime.

### What actually shipped

| Increment | Commit | Note |
|---|---|---|
| A0 metrics | `7062ce6` | `poolObjects`/`worldObjects` **appended** to `TickSample` and the CSV header; `analyse.py` reads with `DictReader` so appending is safe. `@@FINAL` gained `objPool=`/`objWorld=`. |
| A1 archetype | `de2cd6e` | `mArchetypeID` appended to `StartSimulatingObjectPacket`, defaulted to `0` (= `Cube`) — a real shape, not a sentinel, so a peer running the old build is wrong in shape rather than undefined. |
| A2 directed handoff | `93fad1d` | `SendPacketToServer(id, packet)` replaces the broadcast. |
| A3 construct on arrival | `c8792cb` | Tombstone / pending-destroy check moved **before** construction. |
| A4+A5 region-local world | `8b8855d` | Teardown on send; `CreateObjectGrid` gates construction, never iteration. |
| A6 forwarding table | `8c52fd8` | `mLastKnownOwner`; `ResolveForwardTarget` tries position first, then the table. |

### Two things the design did not anticipate

**A missing packet handler is completely silent.** A2 initially produced `hoSent=41 hoRecv=0` — 41
objects lost with no error and no counter. Server-to-server traffic runs in *both* directions, and
which link a message type travels on is decided by where its handler is registered:
`StartSimulatingObjectInServer` was registered only on the outbound `DistributedPhysicsServerClient`
uplink, while relays are registered on `mDistributedPacketSenderServer`. Sending handoff down the
relay direction meant ENet delivered it to a host with no handler, which drops it without a word.
Fixed by registering the type on both. Any new server-to-server type needs the same check.

**`HandleObjectTransitions` iterated a container its own callee mutates.** `TeardownObject` does
`std::erase(mNetworkObjects, networkObject)`, so the loop invalidated itself the moment A4 made
teardown real. Restructured to collect the transitioning set first, then act on it.

### Verification result

| Check | Before | After |
|---|---|---|
| 2-server shuttle `objPool` | 400 / 400 | **359 / 41** |
| 4-server uniform `objPool` | 400 each | **98 / 107 / 98 / 97** |
| I2 conservation | 400/400 | 400/400 |
| I5 handoff parity | exact | exact (41/41, 97/97) |
| `hoFail` / `hoLate` | 0 / 0 | 0 / 0 |
| I4 without misroute | exact | exact (8851 = 8851) |
| Tier 0 | 62/62 | 62/62 |

**I6 holds.** Per-server pool is now the owned set, not the world.

**Reproducibility survived**, which was the sharp regression detector §6.2 asked for. Re-run at
`-Servers 2 -Objects 400 -Ticks 7200 -Seed 42 -Workload shuttle -HandoffLookahead 300
-EpochAlignUs 500000` as `a5-rep-a` / `a5-rep-b`: the deterministic CSV columns
(`tick, owned_objects, integrated_objects, handoffs_{sent,received,failed}, pool_objects,
world_objects`) are **byte-identical on both servers for all 7200 ticks**, and both `@@FINAL` lines
match exactly. Removing the pre-seed model did not reintroduce any ordering dependence.

Note the wall-clock asymmetry this exposes: server 0 needs 103.7 s to run the 7200 paced ticks it
simulates in 60 s, server 1 needs 60.3 s. `shuttle` is deliberately adversarial (359 vs 41 objects),
so server 0 is the one that cannot hold pace. Determinism is unaffected — that is the point of pacing
to a shared clock — but it is the load-imbalance figure the repartitioning increment has to beat.

### Known residual, and what it turned out to be

The I4 shortfall of exactly −1 recorded against the interactions spec was **not** relay-hop latency.
That inference is withdrawn.

A7 appeared to regress I4: three baseline spawn/destroy runs balanced exactly, two A7 runs were short
by 2 and by 1. Adding a per-command send/receive trace made it exact 4/4 and showed every sent
sequence arriving — a loss that disappears under instrumentation is a race, not a logic error, and
A7 changes per-tick work on peers, which changes timing.

The cause is in `GameClient::Disconnect`, and it predates all of this work. `enet_peer_disconnect`
calls `enet_peer_reset_queues`, which discards every outgoing reliable command not yet sent **and
every sent one still awaiting acknowledgement**. Reliable delivery therefore does not survive the
sender's own shutdown: whatever the client sent in its last few milliseconds was dropped on the
floor. `enet_peer_disconnect_later` is the API that holds the peer open until the queues drain.

The same call also serviced the host once and reported failure on any event that was not the
disconnect, so a clean shutdown routinely printed `Failed to disconnect from the server` — which is
why the real fault was never visible in the logs.

Fixed in `4c4fe52`. After it, with A7 in place:

| Configuration | Result |
|---|---|
| spawn + destroy + impulse, 3 runs | exact (1058, 1070, 1062) |
| `--misroute-every 2`, 2 runs | exact (894, 903) |
| impulse only | exact (880) |

The misroute case had never balanced before. **I4 now has no known residual.**

### Reproducibility, re-verified with A7

`a7-rep-a` / `a7-rep-b` at `-Servers 2 -Objects 400 -Ticks 7200 -Seed 42 -Workload shuttle
-HandoffLookahead 300 -EpochAlignUs 500000`: deterministic columns byte-identical on both servers
across all 7200 ticks, `@@FINAL` identical. Tier 0 62/62.
