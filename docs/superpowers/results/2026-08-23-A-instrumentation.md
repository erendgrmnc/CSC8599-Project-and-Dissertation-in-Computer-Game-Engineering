# Phase A results: instrumentation and harness (2026-08-23)

Plan: `docs/superpowers/plans/2026-08-23-phase-a-instrumentation.md`
Spec: `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` §2

## Step 0 — the baseline

`runs/` was empty at the start of this phase (spec §1.1), so this baseline was
generated rather than cited. Every no-op gate below compares against it.

| | |
|---|---|
| Commit | `d8aec0b42b4c1d5c6f14158f7268647baadf7e71` (short `d8aec0b`) |
| Configuration | 2 servers, 400 objects, `uniform`, 1,800 paced ticks, seed 42, `--halo-width 8`, halo reliable, `--drain-seconds 5` |
| Build | Release, via `tools/build-deploy.ps1 -Config Release` |
| Invariants | `analyse.py` exit code 1 — one **INVARIANT FAILURE**: `ticks1800-r1: ownership_gap_ticks = 84 (expected 0)`. Everything else passed: two server CSVs found, `owned` counts 201/199 with 0 mismatch, halo `haloLate=0`/`haloAhead=0` on both servers, `hoFail=0`/`hoReclaimed=0`/`hoDup=0` on both servers. |

**On the ownership-gap failure.** This run used the default `--handoff-lookahead 0`
(not overridden by the Step 3 command), which per `CLAUDE.md` is the case where
"`ScheduleOutgoingObject` releases on send and nobody owns the object for one
network round trip" — a documented, default-behaviour gap, not a regression
introduced by this task. It is recorded here rather than treated as a reason to
re-run: this document's job is to capture what commit `d8aec0b` actually produces
under this exact configuration, so that Task 6's later diff is against a real
baseline rather than an idealised one. Any later run at the same configuration
that reports a materially different `ownership_gap_ticks` is the signal Task 6
exists to catch.

**Working tree at capture time.** `git status --porcelain` was clean before Step 2
(build). Configuring CMake (both the distributed-role pass and the client pass
inside `tools/build-deploy.ps1`) regenerates project files that embed the
configuring machine's absolute paths and CMake-assigned GUIDs, so the tree is
dirty by the time the baseline run executes and `experiment.json` records
`gitDirty: true` / `analyse.py` prints `(DIRTY - not reproducible)`. This is
expected on any machine other than the one the committed files were generated on
and is not addressed here. The four dirty paths, all CMake-generated and none
committed by this task:

- `DistributedPhysicsSystem.sln`
- `DistributedGameServer/DistributedGameServer.vcxproj.filters`
- `DistributedPhysicsManager/DistributedPhysicsManager.vcxproj.filters`
- `PhysicsServerMidware/PhysicsServerMidware.vcxproj.filters`

Task 6's gate compares a baseline run against a post-change run and both are
dirty in the same way from the same regeneration step, so this flag cannot mask
a real difference between the two.

`@@FINAL` lines are stored at `runs/exp-phaseA-baseline/FINAL-baseline.txt`.

## The no-op gate, and why it is not an equality check

Phase A claims to be measurement-only. The obvious verification — re-run the baseline
configuration and require every `@@FINAL` field to match — **is not available on this
machine.** Four clean runs at the identical commit, seed and configuration disagree on
`contacts` (~1%), `snapSent` (~1.5%), halo object counts (~10%), the `hoSent`/`hoRecv`
split (±1), and the per-server object split (201/199 vs 200/200). `ownership_gap_ticks`
across those four runs was 84, 82, 91, 84.

This contradicts `CLAUDE.md`'s claim that under `--run-ticks --fixed-step` "end state
and conservation then reproduce exactly". Conservation does reproduce — the object
total is 400 on every run. End state does not.

What the gate checks instead:

| check | result |
|---|---|
| 21 stable counters identical across all runs, both sides | **FAILED** — `haloLate` was `[0]` pre-change, `[0, 11, 15]` post-change (2 of 3 post-change repeats, one server each) |
| Object conservation total unchanged (400) | pass |
| Varying fields inside their pre-change spread | pass, with one exception noted below |
| Counter read is once-at-exit, off the tick path | confirmed at `DistributedGameServer/ServerStarter.cpp:278` |

**The gate-compare.py verdict is FAILED, not a formality caught by inspection.**
Three post-change repeats (`runs/exp-phaseA-gate`) were taken at the exact baseline
configuration, each individually validated clean by `analyse.py` (no
`REPRODUCIBILITY WARNING`, `ownership_gap_ticks` 86/92/90 — inside the documented
80–95 band, not the thousands that mark a degraded run). Getting to three clean
repeats required discarding five earlier attempts that showed custody firing
(`hoResent`/`hoCustody` nonzero) and multi-second wall-clock overrun against the
15 s simulated budget, per Step 2's explicit discard criterion. None of that
discarding touched `haloLate`, which is not one of Step 2's named degradation
signals — the three kept repeats are the honest clean set.

Against that honest set, `tools/gate-compare.py` reports:

```
GATE FAILED (1):
  haloLate: was [0], now [0, 11, 15]
```

`haloLate` was exactly 0 on all 8 server-run pairs in the pre-change data (baseline
+ repro3) and is one of the 21 fields the brief's own measurement found "identical
every time" across those four clean runs. In the post-change gate set it is 0 in 4
of 6 server-run pairs and 11 or 15 in the other 2 — always the "other" server in a
run where the first server's `haloClamp`/halo timing happened to bind tighter, and
always small relative to `haloSent`≈1,800 per run (0.6–0.8%). No run showing a
nonzero `haloLate` also showed custody firing, ownership-gap blowup, or wall-clock
overrun, so it is not explained by the same degradation mode documented above —it
looks like the same category of machine-timing noise the pre-change table already
documents for `contacts`/`snapSent`/halo object counts, just landing on a field
that a 4-run pre-change sample happened not to catch varying. That is a plausible
explanation, not a proof, and it is exactly the kind of gap Step 5 exists to bound
independently of the numbers. The gate is reported as **FAILED** on its own terms:
re-running until `haloLate` reads 0 would have been cherry-picking a result that
the validity criterion in Step 2 does not justify discarding for.

**Step 4 — varying fields against the pre-change spread**, read from the same
three clean gate repeats (`runs/exp-phaseA-gate`, 6 server-run pairs):

| field | pre-change range (4 runs) | post-change range (3 runs) | verdict |
|---|---|---|---|
| `contacts` (per server) | 171,536 – 173,921 | 171,911 – 174,738 | max +817 (+0.5%) over the pre-change top — negligible |
| `snapSent` (per server) | 246,833 – 251,275 | 248,131 – 252,377 | max +1,102 (+0.4%) over the pre-change top — negligible |
| `haloObjSent` (per server) | 16,149 – 20,186 | 17,444 – 22,310 | max +2,124 (+10.5%) over the pre-change top — same order of magnitude as the pre-change spread itself (~22% wide), not a jump to a different regime |
| `hoSent` + `hoRecv` (both servers, per run) | 78 – 80 | 80 – 81 | +1 over the pre-change top — matches the documented ±1 handoff-count noise |
| `objHalo` (per server) | 7 – 14 | 7, 9, 9, 14, 14, 15 | max +1 over the pre-change top — negligible |
| `hoClamp` (server 0) | 7 – 8 | 8, 8, 8 | inside the band |

No field moved an order of magnitude. The largest relative move (`haloObjSent`,
+10.5% over the prior top) is the same field the pre-change table already flags as
the noisiest (~10% run-to-run), so this reads as more of the same noise rather than
a new effect.

**Step 5 — the structural argument.** Verified by reading the actual call site,
not just grepping for the name:

- `DistributedGameServerManager::GetNetworkByteTotals()` is called exactly once in
  non-test code, at `DistributedGameServer/ServerStarter.cpp:278`, after the run
  loop and drain loop have both exited (the drain loop above it steps the network
  only, not the world) and immediately before the block that prints `@@FINAL`.
- The method (`DistributedGameServerManager.cpp:907-927`) is `const`-qualified and
  performs only reads: `mDistributedPacketSenderServer->GetTotalSentData()` /
  `GetTotalSentPackets()` for the sender host, plus a loop over
  `mDistributedPhysicsClients` summing each peer link's `GetTotalSentData()` /
  `GetTotalSentPackets()`. Those in turn (`CSC8503CoreClasses/NetworkBase.cpp:9-16`)
  are `const` accessors over `NetworkBase`'s own byte/packet counters.
- No other call site exists outside `tools/InteractionTests/NetworkCountersTests.cpp`
  (a unit test constructing a fresh, never-networked `NetworkBase` and asserting the
  counters start at 0). Nothing on the tick path calls `GetNetworkByteTotals()`, and
  no code resets the underlying counters — they are plain accumulators incremented
  by `NetworkBase`'s send path and never zeroed.

All three claims in the brief's Step 5 checklist hold.

**What this does and does not establish.** It rules out a Phase A change that breaks
conservation, that shifts a noisy field beyond its natural spread by an order of
magnitude, or that reads network counters anywhere but once at exit off the tick
path. It does **not** rule out a change that perturbs those fields within their
spread, and on this run it did not rule out `haloLate` moving off zero — the gate's
own stable-set check reports that as a failure, honestly, rather than being
re-run away. That is a weaker claim than the plan originally intended, and it is
stated here rather than papered over.
