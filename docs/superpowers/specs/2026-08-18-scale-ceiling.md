# What the system actually scales to (2026-08-18)

Written in answer to a direct question: the measurements so far are all at a few hundred objects,
and the thesis is about worlds far larger than that. This records what the ceiling really is, what
was holding it down, and what stands between here and a million objects.

---

## 0. Every prior measurement was a Debug build

`tools/build-deploy.ps1` defaults to `-Config Debug`, and every number quoted in the previous
increments came from one. MSVC Debug disables inlining and enables checked iterators, so the physics
loop runs several times slower than shipped code. **Every absolute performance figure in the earlier
specs is pessimistic by an unknown factor and should not be quoted in the paper.**

The same 400-object `shuttle` run, 1,800 paced ticks:

| | busiest server, wall clock |
|---|---|
| Debug | 33.1 s |
| Release | **15.13 s** for 15 s simulated |

In Release the busiest server holds real time exactly, so the *pacing* is the limit and the physics
has headroom the Debug run could not show. `haloLate` also drops to 0 on both servers, which means
the halo's reproducibility failure under `shuttle` — reported in the halo spec §3.5 as a consequence
of load imbalance — was **substantially a Debug artefact**. The load-imbalance argument still stands
at higher object counts, but the specific 65,307-late-update figure was measuring the wrong thing.

All figures below are Release, AMD Ryzen 5 5600X (6 cores / 12 threads), one machine.

---

## 1. The ceiling was one loop

`PhysicsSystem::BroadPhase` tested **every pair** of dynamic objects: two nested loops over
`mDynamicObjectList`. That is O(n²) in the objects a *server* owns, and it dominated everything.

Measured on one server, `uniform`, Release:

| objects | physics ms/tick (before) | ratio |
|---|---|---|
| 1,000 | 8.63 | — |
| 2,000 | 36.72 | **4.26x for 2x the objects** |
| 4,000 | did not complete | |

An exponent of 2.09. Contacts only doubled across that range (1,441 → 2,892), so the cost was pair
*enumeration*, not contact resolution.

The tick budget at 120 Hz is 8.33 ms, so the per-server ceiling was **about 950 objects**.

### The fix

A uniform grid over XZ, rebuilt each tick. XZ rather than XYZ because the pair test already forced
both half-extents to 1000 on Y — the broadphase has always been two-dimensional in effect.

| objects | physics ms/tick (after) | vs 1,000 |
|---|---|---|
| 1,000 | 1.151 | 1.00x |
| 2,000 | 2.274 | 1.98x |
| 4,000 | 4.756 | 4.13x |
| 8,000 | 10.601 | 9.21x |

Exponent **1.07** — linear. At 2,000 objects it is **16x faster**. Per-server ceiling moves from
~950 objects to **~6,000**.

### A second bug it exposed

The old inner loop was `for (int j = i; ...)` — starting at `i`, not `i + 1`. Every dynamic object
was therefore paired **with itself** every substep, and that pair was resolved as a contact.

This is why contacts/tick at n=1,000 fell from 1,441 to 441 when the grid landed: exactly 1,000
fewer, one per object. Every contact figure in the earlier specs is inflated by one per object per
substep, and the claim in the halo spec that a particular workload "produces only floor contacts" was
wrong — they were self-contacts.

---

## 2. What holds at scale, and what does not

### Correctness holds

4 servers, 32,000 objects, world ±600, 600 paced ticks:

| check | result |
|---|---|
| handoff parity | 1,682 sent = 1,404 received + 278 pending — **exact** |
| `objPool` per server | ~8,000 against `objPreseed` 32,000 — **I6 holds** |
| `hoFail` | 0 |
| `hoLate` | 0 |

The distribution machinery — region-local state, construct-on-arrival, atomic ownership transfer,
the forwarding table — works unchanged at 80x the object count it was developed against.

### Wall-clock scaling cannot be claimed from these runs

Every "N server" run puts N server processes, plus the manager, midware and a client, on **one
6-core machine**. A 4-server run at 16,000 objects is 16,000 objects on one PC; the 1-server run at
4,000 is 4,000 objects on the same PC. The former is doing four times the total work on the same
silicon, so comparing their wall clocks measures contention, not distribution.

That is why the 4-server run costs ~9.5 s per 5 s simulated at 4,000 objects each, where a single
server with the same 4,000 costs ~5.3 s.

**The architectural claim that these runs do support** is the locality one: per-server state and
per-server physics cost scale with region occupancy, not world size. That is I6, and it is measured
directly. A wall-clock speedup claim needs one server per machine and is not something this
single-machine harness can produce.

---

## 3. The road to a million

At ~6,000 objects per server, a million objects needs ~167 servers. Three things stand in the way,
in the order they will bite.

### 3.1 A hard cap of 20 servers

`StartDistributedGameServerPacket` carries `int serverIDs[20]`, `char borders[20][256]`,
`connectedServerIDs[20]`; `DistributedRepartitionPacket` carries `MAX_REGIONS = 20`. Twenty servers
is a wire-format limit, so the current protocol tops out near **120,000 objects** however much
hardware is available.

Raising it is not just a bigger array: `borders[20][256]` is 5 KB of a single packet already, and a
fixed array sized for 200 servers would be 51 KB in every bootstrap message. The partition needs to
be sent incrementally, one region per packet or a page at a time.

### 3.2 Snapshot traffic is O(world) per client

Each server broadcasts a full snapshot of **every object it owns** to **every client**, one packet
per object, at 10 Hz. At 6,000 objects per server that is 60,000 packets per second per client from
one server. At a million objects across 167 servers it is ten million packets per second per client.

This is the single largest remaining obstacle, and it is the increment listed as out of scope
throughout: **interest management**. A client needs the objects near its own viewpoint, not the
world. Until that exists the system can *simulate* a large world but cannot *serve* one.

Note this is a client-facing limit, not a simulation limit — the server-to-server traffic (handoff,
halo) is already proportional to border area rather than world size.

### 3.3 Per-server structures that still grow with objects touched

`mLastKnownOwner` (reported as `objFwd`) holds an entry per object this server has ever handed away.
It no longer scales with *world* size — that was fixed — but it grows monotonically with cumulative
handoff traffic and is never pruned. At 32,000 objects it was ~700 entries per server after 5 s;
over hours it would grow without bound. It needs an eviction policy keyed on the same staleness
argument the halo retirement uses.

---

## 4. What to measure next, and how

1. **Multi-machine.** Even two physical machines would separate distribution overhead from CPU
   contention and make a speedup claim defensible. Everything needed is already in the deployment
   tooling; only the run configuration changes.
2. **Per-server capacity curve in Release**, with the halo on, to find the real object budget per
   server rather than the ~6,000 measured with it off.
3. **Density rather than count.** The grid broadphase is linear in objects but its constant depends
   on objects *per cell*. A million objects spread thinly is cheap; a hundred thousand in one heap is
   not. The workloads should sweep density explicitly, since that is what a real game world varies.
