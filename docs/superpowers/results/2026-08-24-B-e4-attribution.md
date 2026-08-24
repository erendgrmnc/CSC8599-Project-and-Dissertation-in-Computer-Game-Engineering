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
