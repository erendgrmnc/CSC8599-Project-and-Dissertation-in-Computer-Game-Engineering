# Item 13 counterfactual — is the snapshot-throughput change real, and is it a regression?

**Date:** 2026-08-24
**Builds:** published `02e306b` (E8's own commit) vs `012dee8` (Phase A merged, plus the item-14
harness fix — neither of which touches snapshot emission).
**Machine:** ERENDEGIRMENCI, 13th Gen Intel i7-13650HX, 14 cores, Windows 11 Pro.

`docs/EVALUATION.md` item 13 recorded a snapshot-throughput change between the published E8 commit
and Phase A's re-measurement — 3.6x at radius 0, and not uniform across radii (1.36x/2.10x/3.08x at
25/50/100). It was inferred by comparing two measurement **sessions taken days apart**, and the
document was explicit that it could not rule out session-level drift by argument alone. It was the
phase's highest-priority follow-up because E8's verdict is stated as build-scoped partly on its
account.

This settles the first half of the question and narrows the second.

---

## What was done

Both builds were compiled Release from clean checkouts and run **back to back in one session on one
machine**, so no cross-session variable survives. Configuration is exactly `exp-bytes-1client`'s:
2 servers, 4,000 objects, 20 s realtime (`-Ticks 0`), seed 42, `uniform`, world +-150,
`--halo-width 8 --halo-lookahead 4 --drain-seconds 0`, 1 client, radii 0/25/50/100, 3 repeats.

The server-side invocation is byte-identical between the two harness versions at this configuration
(`$custodyArg` is empty and `$Clients` is 1), so each tree ran its **own** `measure.ps1` — the old
side therefore reproduces the published measurement exactly, with no harness backport.

Three further controls were then run to localise the effect. Raw data: `runs/exp-cf-new`,
`runs/exp-cf1-new`, `runs/exp-cfst-new`, and the same three names under `C:\wt02e\runs` for the
published build. Comparison script: `cf_compare.py` (scratchpad; not a deliverable).

---

## Result 1 — the change is real and reproduces

| radius | snap/s published | snap/s current | ratio | ratio recorded in item 13 |
|---|---|---|---|---|
| 0 | 62,374 | 218,475 | **3.50** | 3.6 |
| 25 | 14,404 | 26,042 | **1.81** | 1.36 |
| 50 | 25,493 | 48,715 | **1.91** | 2.10 |
| 100 | 44,230 | 139,287 | **3.15** | 3.08 |

Three of four radii land within 0.1 of the recorded figure. Radius 25 does not (1.81 vs 1.36), and
it is the one point that is noisy on **both** builds — see Result 4.

**Item 13's cross-session inference was sound.** The change is a property of the builds, not of the
sessions they were measured in. The document's fallback argument — that near-identical halo
throughput across sessions proved tick rate was unchanged — is now unnecessary rather than merely
persuasive.

## Result 2 — it is in the snapshot path, not in halo, interest, peers, or tick rate

The 2-server configuration confounds several mechanisms at once. Two controls remove them:

| control | published | current | ratio |
|---|---|---|---|
| 1 server, `--halo-width 0`, radius 0, `uniform` | 630.0 snap/tick | 2000.0 snap/tick | **3.17** |
| 1 server, `--halo-width 0`, radius 0, `cluster` (stationary) | 751.5 snap/tick | 2000.0 snap/tick | **2.66** |

With one server there is no halo band, no peer link and no handoff. At radius 0 interest management
is off and `snapSupp` is **0 on both builds**, so nothing is being filtered. The effect survives all
of that. It also survives on a stationary workload, so it is not a property of how much the objects
move — which rules out the obvious "the deltas now carry the changes they always should have"
explanation.

Note the current build ran **fewer** ticks than the published one in the moving 1-server control
(1,251/1,354/1,360 against 1,551/1,506/1,492) while sending 2.8x more. Whatever changed costs time
rather than saving it, which is what a tick-rate explanation would need to be false — and it is.

## Result 3 — the current build emits a constant per tick; the published build does not

Across every configuration tested — 2 servers and 1, moving and stationary, 4 radii — the current
build emits **exactly 2000.0 object-snapshots per counted tick**:

```
1 server, uniform   : 2,504,000/1251 = 2001.6   2,708,000/1354 = 2000.0   2,720,000/1360 = 2000.0
1 server, cluster   : 2,164,000/1081 = 2001.8   2,052,000/1026 = 2000.0   1,992,000/ 996 = 2000.0
```

The published build emits 630-755 per tick, varies with load, and varies between repeats at the same
configuration. A constant is the signature of a schedule being met; a load-dependent number is the
signature of one being missed.

## Result 4 — the current build is far more reproducible

Within-radius spread of snap/tick across 3 repeats, 2-server sweep:

| radius | published | current |
|---|---|---|
| 0 | **130.6%** | **0.3%** |
| 25 | 560.0% | 47.3% |
| 50 | 98.6% | 4.9% |
| 100 | 9.6% | 1.7% |

The published build's server-1 tick counts collapse without warning — 513, 757, 305 and 1,001
against a normal ~2,390 in the same experiment — while its `snapSent` stays in range. That is a
server failing to accumulate ticks, not a measurement artefact, and it is the direct cause of the
radius-25 disagreement in Result 1: at that radius the published build's own repeats span 60% on raw
totals, so any single ratio drawn from them carries that width.

**This is the finding with the most consequence for E8.** The counterfactual that E8's verdict was
hedged against — "the published build would have failed at radius 100" — is computed against a build
that is erratic in precisely the quantity E8 measures, by up to 130% at the radius the comparison
turns on. It is a weaker challenge to E8's verdict than it appeared when the two builds were assumed
equally well-behaved.

---

## What is NOT established

**Which commit.** The obvious candidates were checked and eliminated by inspection: the snapshot
cadence line (`mTimeToNextPacket += 1.0f / 60.f`) is byte-identical, so is `BroadcastSnapshot`, so is
the delta-baseline logic around `mServerSideLastFullID`, and so is the counting site itself
(`mSnapshotsSent += targets.size()`). `NetworkObject`'s diff is a default constructor and a removed
`std::cout`; `DistributedPacketSenderServer`'s is a peer-left callback. None of these changes
snapshot volume. The cause is therefore downstream of what a diff of the obvious files shows —
most likely in what `targets` resolves to, or in how often the schedule fires — and locating it
needs a **bisect over the 36 non-doc commits in the range**, roughly six build-and-measure cycles.

(Item 13 says "roughly 18 core commits". The range `02e306b..2060f55` holds **61** commits, 36 of
them non-doc.)

**Regression or fix.** The evidence leans towards the published build being the defective one — it
misses a schedule the current build meets exactly, it is erratic where the current build is stable,
and its servers intermittently fail to accumulate ticks. But "leans towards" is not a verdict, and
one is not offered here. A build that sends 3.5x more snapshots for the same simulated world is
either delivering updates it previously dropped or sending ones nobody needs, and the two are
distinguished by what the **client** receives — which needs client-side instrumentation this phase
does not have.

## Recommendation

Bisect the range on the 1-server, `--halo-width 0`, radius-0 control (Result 2). It is the cheapest
configuration that shows the effect at full strength, needs no halo or peer set-up, has a
zero-suppression baseline, and gives a clean pass/fail signal per commit: 2000.0 snap/tick or not.
