# Phase B — attributing the E4 conservation regression (item 12)

**Date:** 2026-08-24
**Commit:** `8b8d0e3` (HEAD at the time of the runs)
**Outcome:** item 12 closes. There was never an object loss. The regression was an
artefact of the conservation check reading the wrong field, compounding an end-of-run
truncation that a long enough drain fully recovers.

Spec: `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` §3. Its §3.0 said
**do not start with the bisect** — first test the cheaper explanation, because "this ordering
costs one run to potentially delete an entire phase." It did. §3.1 and §3.2 never ran, and no
bisect was needed.

---

## The pre-check (§3.0)

E4's regressing configuration re-run at HEAD: `cluster`, 4,000 objects, 2 servers, 7,200 paced
ticks, `--handoff-lookahead 300`, rebalancing every 400 ticks, 3 repeats — with the drain
raised from its 5 s default.

`analyse.py` reported it **worse**, not better: `conservation_delta` of −2244 / −2007 / −2013
against item 12's recorded −198 / −703 / −84.

The counters said something different from the verdict. On `rebalanceInterval400-r3`:

| server | hoSent | hoRecv | hoCustody | objs | objPool |
|---|---|---|---|---|---|
| 0 | 3038 | 1 | 315 | 963 | 963 |
| 1 | 1 | 1025 | 0 | 1024 | 2722 |

`3038 − 1025 = 2013`, exactly the reported loss. But server 1 **holds 2,722 objects while
reporting 1,024**, and `963 + 2722 + 315 = 4000`. Nothing was missing.

## What `objs` actually is

`objs` is `Profiler::GetObjectsOnBorders()`. `ServerWorldManager::Update` writes it
(`ServerWorldManager.cpp:1126-1137`) as `activeObjCount` — the number of `mTestObjects` that
report `HasPhysics()`. Two properties make it unusable as a conservation quantity:

1. It is written **inside `Update`**, and the drain phase runs the loop *without stepping the
   world*. On any run ending with transfers in flight it is frozen at the last stepped tick
   while the drain is still installing arrivals.
2. It counts active simulation participants, which is not the set of objects a server is
   responsible for.

The per-tick CSV makes the freeze visible. On the 120 s-drain run, at the final stepped tick
(7199) the two servers hold 732 and 1,024 — total **1,756**, exactly what `objs` reports at
exit. The `@@FINAL` line for the same run reads `objPool` = **4,000** and `hoCustody` = **0**:
the drain recovered all 2,244 transfers that were in flight when the run ended.

So the run was perfectly conserved *and* fully drained, and the check still called it a
2,244-object loss — because `objPool` is read live at print time, after the drain, while `objs`
was last written before it.

## Verification across every dataset in the repository

| conservation counted on | runs failing (of 78) |
|---|---|
| `objs` (the old check) | **5** |
| `objPool + hoCustody` | **0** |

78 runs across 13 experiments — every dataset present, including all of Phase A, the E8/E3
sweeps and the item-13 counterfactual. `hoCustody` is **added** rather than assumed to sit in
some pool: a transfer in custody has been released by the sender and not installed by the
receiver, so it is in neither server's `objPool`. That is the §0.7 ownership gap appearing in
the accounting, which is why it is counted separately rather than folded in.

## The controls

| configuration | conservation on `objPool + hoCustody` | note |
|---|---|---|
| `--handoff-lookahead 300`, drain 30 s | exact, 3/3 | custody 553/316/315 at exit |
| `--handoff-lookahead 300`, drain 120 s | exact, 1/1 | custody **0** — fully drained |
| `--handoff-lookahead 0`, drain 30 s | exact, 3/3 | custody 0; old check failed 1 of 3 |

The lookahead-0 control is the one that shows why this was never diagnosed from the summary
alone: on the old check the same configuration failed *intermittently* — one repeat of three —
which reads like a race rather than like a measurement error.

## Changes made

- `tools/analyse.py`: `conservation_delta` is now `(objPool + hoCustody) − (preseed + spawned −
  destroyed)`. Adds `drain_recovered` (`objPool − objs`), reported not checked, so the quantity
  the old check mistook for a loss stays visible.
- `tools/test_analyse.py`: `ConservationFieldTests`, four tests pinning the **field choice** —
  the failure mode is reading a stale counter, so only a fixture where `objs` and `objPool`
  disagree can catch it. Verified adversarially: reverting to `objs` breaks three of them.
- `tools/gate-compare.py`: `objs` kept in `CONSERVED` — that gate compares two runs of the same
  configuration against each other, not against a world total, and nothing is outstanding at
  exit on its healthy-path configuration — but commented with the hazard and an instruction to
  drop it first if the gate is ever pointed at a rebalancing or high-lookahead configuration.

## Consequences beyond item 12

**The E7/E4 reading is now reconciled.** `EVALUATION.md` treated an exact `ho_parity_delta`
match as *explained* (end-of-run truncation) for E7 and *unexplained* for E4. It is the same
mechanism in both. §3.3 flagged this as needing reconciliation if §3.0 closed the item; it did.

**No published figure changes.** E4's own verdict was measured on `exp-balance`, where
conservation was exact under the old check too. Item 12 concerned `exp-fix4-E4` only.

**One residual, genuinely open.** `ownership_gap_ticks` runs at 5,952–6,499 of 7,200 on these
rebalancing runs, and two repeats showed `ownership_double_ticks` (5 and 1) — two servers
claiming one object. Conservation being exact does not touch that: nothing is lost, but
ownership is not atomic during a partition move. This is Phase D's subject (§0.7, item 2), and
these runs give it a much sharper test case than the ones on record — `--handoff-lookahead 300`
was supposed to make ownership atomic and demonstrably does not under rebalancing.

## What was not done, and why

The three-point bisect of §3.2 (`93e6f21`, `776115b`, `02e306b`, HEAD) was **not run**. Its
premise — that a code change caused an object loss — is false: there is no loss to attribute.
Four builds and twelve runs saved.

---

# Addendum — the root cause, and a second defect it was hiding

Written after the audit that followed this attribution. The field-choice fix above was
correct but treated a symptom. The cause is more general.

## `@@FINAL` was reporting two different instants at once

`ServerWorldManager::Update` publishes its running counters into `Profiler` at its tail
(`ServerWorldManager.cpp:1199-1205`). The drain phase deliberately does **not** step the
world, so `Update` never runs during it — while `DrainScheduledArrivals()` keeps installing
arrivals and incrementing `mHandoffsReceived`.

The `@@FINAL` line therefore mixed:

| source | fields | state |
|---|---|---|
| `Profiler::Get*`, published in `Update` | `objs`, `hoSent`, `hoRecv`, `hoFail`, `hoLate`, `haloLate`, `haloAhead` | **frozen at the last stepped tick** |
| locals read at print time | `objPool`, `objWorld`, `objFwd`, `objHalo`, `hoCustody`, `hoPending`, `hoSched`, `hoResent`, `hoReclaimed`, `hoClamp`, `hoDup` | post-drain |

Every invariant computed across that boundary was comparing two different moments. Item 12's
conservation failure was one consequence; **invariant I5 (handoff parity) was another**, and it
had the same character — a real-looking failure with no real cause.

Counters published from `DistributedGameServerManager` (`snapSent`, `snapSupp`, `cmd*`, `halo`
send/recv, `objSpawned`, `objDestroyed`, `manifestSent`) are **not** affected: the drain loop
calls `UpdateGameServerManager(dt)`, so they keep being refreshed. The item-13 counterfactual
is also unaffected — those runs used `--drain-seconds 0`, so no drain phase existed.

**Fix:** `ServerWorldManager::PublishCounters()`, called at the end of `Update()` as before and
**again after the drain**, before `@@FINAL` reads anything. `objs` is deliberately *not*
republished: it is `activeObjCount`, computed by walking `mTestObjects` inside `Update`, so
there is no stored value to re-emit — and it is not a conservation quantity. Leaving it stale
is honest, since what it means is "objects being simulated as of the last stepped tick".

**Effect, same configuration, before and after:**

| | server 0 | server 1 |
|---|---|---|
| before | hoSent 3269, hoRecv 1 | hoSent 1, **hoRecv 1025** |
| after | hoSent 2130, hoRecv 111 | hoSent 111, **hoRecv 2130** |

Both directions now match exactly, and `ho_parity_delta` is 0 where it previously read 2244.

## The second defect: custody resends can DUPLICATE objects

With the counters correct, a re-run of the same configuration (3 repeats, 120 s drain) gives:

| repeat | objPool sum | hoSent ↔ hoRecv | hoResent | hoDup | conservation |
|---|---|---|---|---|---|
| r1 | 4000 | 2241 ↔ 2241 | 1 | 1 | exact |
| r2 | 4000 | 2717 ↔ 2717 | 0 | 0 | exact |
| r3 | **5303** | 10780 ↔ 10780 | **3471** | **3391** | **+1303** |

r3 ends with **1,303 more objects than the world contains** — the two pools hold 5,303 between
them. This is not a counting artefact: `conservation_delta` is *positive*, which the old
`objs`-based check could never produce, and it appeared only once the counters told the truth.

The mechanism is the one `DistributedGameServerManager`'s own comment anticipates for resends:

> a receiver that STILL HOLDS the object re-applies it harmlessly and acks — but one that has
> since handed the object onward re-installs an object that now lives elsewhere, so resends are
> bounded by `--handoff-max-attempts` rather than repeated indefinitely

Under rebalancing that bound is not sufficient. 3,471 resends produced 3,391 duplicate arrivals
and 1,303 surviving duplicate objects. **Duplication is a worse failure than loss** for a
physics simulation: the object exists twice, is integrated twice, and collides with itself.

Filed as backlog item 15. It belongs to Phase D with the rest of the ownership work, and it is
a stronger argument for doing that phase than the ownership gap alone.

## Audit status of the other invariants

| invariant | field(s) | verdict |
|---|---|---|
| I1 ownership gap / double owner | CSV `owned_objects` (= `activeObjCount`) | **sound** — verified equal to `pool_objects` on every tick at lookahead 0 and on the Phase A baseline; 56 of 7,200 ticks divergence on one server at lookahead 300 |
| I2 conservation | `objPool` + `hoCustody` | **fixed** (this document) |
| I4 command accounting | `cmd*` from `DistributedGameServerManager` | sound — refreshed during the drain; separately made non-vacuous on 2026-08-24 |
| I5 handoff parity | `hoSent`/`hoRecv` | **fixed** by `PublishCounters()` |
| I3 resurrections | client `@@FINAL` | sound, and only actually evaluated since the item-14 fix |
