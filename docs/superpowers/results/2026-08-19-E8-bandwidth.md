# E8 — bytes, and the Dyconits composition claim

Date: 2026-08-19 / 2026-08-20
Runs: `runs/exp-bytes` (realtime), `runs/exp-bytes-paced` (paced), `runs/exp-halo-diag` (targeted
diagnostics). Packet sizes from commit `abf5c37` (`tools/InteractionTests/PacketSizeTests.cpp`). No
file under a frozen-source path (`DistributedGameServer/`, `DistributedPhysicsManager/`,
`PhysicsServerMidware/`, `CSC8503CoreClasses/`, `CSC8503/`) was changed at any point in this task —
everything below came from reading frozen source and running the existing binaries with existing,
already-supported CLI flags.

## Verdict, stated up front

> **The composition claim — "the halo is paid for out of interest management's saving" — is
> UNTESTED. It is neither refuted nor established.** Two independent defects in the measurement
> harness (detailed below) contaminate the counters this document's bytes/second figures are built
> from, on both the halo side and, provisionally, the snapshot side. The specific bytes/s numbers
> this investigation produced (25.7 MB/s and 33.9 MB/s for the halo) are **retracted** — see
> "Retraction," below, for why. A rough geometric estimate suggests the claim would plausibly hold if
> cleanly measured, but that estimate is explicitly not a result and must not be read as one. **E8 is
> blocked on the frozen build** (`PhysicsServerMidware/` and `DistributedGameServer/` are frozen for
> this evidence pass) and is left in this state rather than force-completed. It can be re-run cleanly
> once the two defects below land in a future build phase.

## Retraction

This document previously reported the halo's server-to-server cost as **25,656,033.6 B/s (~25.7
MB/s)** in a realtime configuration and **33,896,259.3 B/s (~33.9 MB/s)** in a paced configuration,
and concluded from those figures that the composition claim was refuted (halo cost exceeding the
interest-management saving by roughly an order of magnitude on the required lower bound). **Both
figures are retracted.** They are not measurements of the halo's per-tick or deployment cost. They
are measurements of "however much an unthrottled, multi-second spin loop can publish on this
machine, divided by a duration that excludes that spin loop entirely" — see "Defect 2" below. The
two figures are kept visible in this paragraph, with this retraction attached, rather than deleted,
because they were already quoted in this session's status reports before the defect was found; a
retracted number that stays visible with its retraction is safer than a silent rewrite.

## Claim under test

Interest management makes each server send snapshots only to clients whose declared view contains
the object, cutting server-to-client traffic. The halo band (`--halo-width`) costs extra
server-to-server traffic to make cross-border collisions work. The claim: **the halo is paid for out
of interest management's saving** — halo bytes/s < the snapshot bytes/s saved by turning interest
management on, checked at the least favourable (lower) bound. This document set out to convert a
previously qualitative (packet-count) version of this argument into bytes/second. It did not
succeed, for the reasons below.

## The two defects, for the build-phase backlog

**Defect 1 — `PublishHaloBand()` has no rate gate.**
`DistributedGameServer/DistributedGameServerManager.cpp:174` calls `PublishHaloBand()`
unconditionally, once per call to `UpdateGameServerManager`, with no throttle of its own.
`BroadcastSnapshot`, by contrast, is explicitly gated to 60 Hz by `mTimeToNextPacket`
(`DistributedGameServerManager.cpp:195`, the `mTimeToNextPacket += 1.0f / 60.f` line — the surrounding
gate starts at line 176). This asymmetry is real and verified directly from source. On its own, it
was checked and found *not* to be the dominant explanation for this experiment's numbers (the main
simulation loop paces identically to 120 Hz in both "realtime" and "paced" configurations in this
harness — see "Investigation history," below) — but it compounds with defect 2: whatever loop calls
`UpdateGameServerManager`, gated or not, the halo publish inside it has nothing slowing it down.

**Defect 2 — `--drain-seconds` is parsed but never forwarded, so it cannot be set through supported
tooling.** `DistributedGameServer/ServerStarter.cpp:214` parses
`config.GetInt("--drain-seconds", 5)` and runs an unthrottled `while` loop (lines 214–235) for that
many real seconds after the timed/paced main loop ends and before the `@@FINAL` totals are printed.
The loop has no sleep and calls `UpdateGameServerManager(dt)` — and therefore the ungated
`PublishHaloBand()` from defect 1 — on every spin iteration, as fast as the CPU allows. Its purpose is
legitimate: letting in-flight handoffs land after the timed loop stops (the surrounding comment
documents an earlier version that, without it, lost 1,008 of 4,000 objects on a cluster workload when
a lightly-loaded server exited mid-handoff). But `PhysicsServerMidware/ProgramStart.cpp`'s
flag-forwarding block — the only code path that can deliver a flag to a spawned game-server process —
does not forward `--drain-seconds` at all (`grep -c drain PhysicsServerMidware/ProgramStart.cpp`
returns 0), despite forwarding over a dozen other game-server flags by the identical
`if (config.Has("--X")) serverExtraArgs += ...` pattern. There is therefore no way to run
`--drain-seconds 0` through `tools/measure.ps1` or `tools/run-experiments.ps1` without editing frozen
midware source. **The fix is mechanical** — add one more `if` block to the existing forwarding list,
matching the pattern already used a dozen times over — but it is not implemented here.

**E8 can be re-run cleanly once both land**, most simply by forwarding `--drain-seconds` and
re-running `exp-bytes-paced` with `--drain-seconds 0`.

**Why this was not fixed in this pass, and why a workaround was not attempted either.**
`PhysicsServerMidware/` is frozen for this evidence pass. The freeze matters specifically here, not
just generally: the drain phase exists to install in-flight handoffs, so changing it can move
`hoSent`/`hoRecv` counts — and `docs/superpowers/results/2026-08-19-E5-soundness.md`'s 120 runs, the
project's headline soundness result, are built on the current binary. Preserving E5's validity is
worth more than completing E8 now. Nor is a workaround (e.g. lengthening the timed loop to dilute the
drain phase's share) viable: the drain loop spins unthrottled while the main loop is paced to 120 Hz,
so drain iterations outnumber main-loop ticks by orders of magnitude regardless of how long the timed
loop runs — demonstrated directly below (the flat-across-a-20x-duration-range result).

## The four packet sizes (given, Task 4, commit `abf5c37`) — unaffected by the defects above

| Packet | payload bytes |
|---|---|
| `FullPacket` | 72 |
| `DeltaPacket` | 24 |
| `HaloObjectState` (one halo entry) | 60 |
| `HaloUpdatePacket` header | 16 |

`TRANSPORT_OVERHEAD = 36` bytes/packet. The conversion arithmetic itself is correct and unaffected —
what's wrong is the *input counters* fed into it, not the formulas:
```
snapshot bytes, UPPER = count * (72 + 36) = count * 108
snapshot bytes, LOWER = count * (24 + 36) = count * 60
halo bytes             = haloSent * 16 + haloObjSent * 60 + haloSent * 36
bytes/s = bytes / measured_duration_s
```

## What still stands: the three-level filtering framework and both savings framings

`snapSupp` counts snapshots that would have been sent and were suppressed, so `snapSent + snapSupp`
at a given interest radius is the pre-filter send-decision count. Three levels — **unfiltered**
(`snapSent+snapSupp` @ radius 0), **peer opt-out only** (`snapSent` @ radius 0), **peer opt-out +
client interest** (`snapSent` @ radius 50) — and two savings framings built from them
(**client-interest-only**, isolating the mechanism actually compared to Dyconits; **all-filtering**,
including the pre-existing, client-independent peer-server opt-out) remain the right way to decompose
this question. The counts and the method are sound. **What is not sound is converting either side's
counts to bytes/second**, because — per the `snapSent` anomaly below — even the snapshot-side counts
this framework depends on are not confirmed free of the same drain-phase contamination that corrupted
the halo side.

## Investigation history

**Round 1** measured a realtime configuration only and reported the halo far more expensive than the
saving, composition claim refuted.

**Round 2** checked a specific confound: `PublishHaloBand` has no rate gate (defect 1), and the
realtime run's main loop was suspected of outrunning the 120 Hz physics substep rate. Checked
directly — `tools/measure.ps1` passes `--fixed-step` unconditionally regardless of `-Ticks` vs
`-Seconds`, and `HeadlessRunner.cpp`'s pacing branch is gated on `reproducible && counting`, not on
bound type — so every `measure.ps1`-driven run paces its main loop to 120 Hz identically. Confirmed
via matching `Headless pacing: REPRODUCIBLE` log lines in both configurations and via raw tick-timing
data (2,395 ticks over 19.957 s = 120.03 Hz, not ~1 kHz). A paced sweep (`-Ticks 1800`) was run to
check empirically; it came back *higher* (33.9 MB/s) than realtime (25.7 MB/s), not the ~8x lower the
loop-spin hypothesis predicted. At the time this was read as confirmation the halo really was this
expensive, independent of pacing. It was not — both figures were dominated by the same, different,
still-undiscovered artifact (defect 2), which is exactly why fixing the main-loop pacing didn't move
them.

**Round 3** checked a second confound, raised against round 2's own arithmetic: the published volume
(~4,472 combined entries/tick) exceeds the absolute ceiling two regions can produce (4,000, since each
object has at most one foreign region with only two servers), and is roughly 20x a geometric estimate
of the real border-band size (~107 objects/server for an 8-unit band on a 300-unit border). Three
specific hypotheses were checked against existing logs, all ruled out:

1. **`mServerBorderMap` corruption** (extra, stale, or degenerate regions) — ruled out. Exactly two
   clean entries logged per server (`Received Border for server 0 -150/0|-150/150`,
   `server 1 0/150|-150/150`), a correctly bisected 50/50 split. Both insertion code paths guard
   against duplicate entries by server id; no repartition ever fired.
2. **Wrong `mHaloWidth` at runtime** — ruled out. `--halo-width 8.000000` confirmed in every server's
   forwarded `argv` in every run checked.
3. **A broken `GetOverlappedServers`/`CollectHaloPublications` predicate** — ruled out. The predicate
   reads correctly on inspection (closest-point-on-rectangle, same geometry as the area-effect blast
   query), and a `--halo-width 1` diagnostic confirmed it empirically: entries/packet came back at
   3.0, not pinned at the 20-entry cap — a broken "matches almost everything" predicate would produce
   full, capped batches, not small floating ones.

All three of the specific hypotheses offered were wrong; what was actually found is defect 2 above,
confirmed as follows.

## The decisive evidence: flat across a 20x duration range

Three short diagnostics (`runs/exp-halo-diag`, 2 servers, 400 objects, `--halo-width 8`,
`--workload uniform`, varying only `-Ticks`, drain phase left at its unforwardable default of 5 s):

| ticks | main-loop duration | combined `haloObjSent` (both servers) |
|---:|---:|---:|
| 60 | 0.5 s | 4,443,170 |
| 240 | 2.0 s | 4,324,026 |
| 1,200 | 10.0 s | 4,437,202 |

Across a **20x range** of main-loop duration, the total varies by under 3% and does not trend with
duration at all — it is lowest at the *middle* duration, consistent with ordinary run-to-run noise,
not a duration effect. If the timed loop's own contribution were significant, 10 s of main loop
should have produced roughly 20x the halo traffic 0.5 s did; it produced statistically the same
amount. This is only explicable if a fixed-duration, unthrottled component (the 5-second drain phase)
dominates the total so completely that the timed loop's own contribution is undetectable above noise
— and it directly demonstrates why lengthening the timed loop cannot dilute the artefact away: drain
iterations already outnumber main-loop ticks by orders of magnitude at even the shortest duration
tested, and that ratio only gets more lopsided as the drain window's *relative* share shrinks.

The same pattern held at E8's actual 4,000-object scale: realtime (20.0 s main loop) vs. paced
(15.04 s main loop) gave combined `haloObjSent` medians of 8,166,741 vs. 8,113,876 — a 25% duration
difference producing a 0.6% total difference. That was round 2's "confirmation." It is better
explained as the same drain-phase artefact dominating both configurations equally.

Estimated drain-phase rate, from the diagnostics above: ≈4.4M combined entries / 5 s ≈ **880,000
entries/sec** (both servers combined, 400 objects, width 8) — tens of thousands of unthrottled
`PublishHaloBand()` calls per second, each publishing a genuinely small, geometry-correct band
(consistent with the entries-per-packet ≈ 3 result at width 1, from the round-3 predicate check
above). This is not a broken predicate publishing too much per call; it is a correct predicate called
an unbounded number of times.

## The snapshot side is also contaminated, and E8's numbers are unusable on both sides — not investigated further

The same three-diagnostic comparison, run on `snapSent` instead of `haloObjSent`: 139,166 (t=60) vs.
139,297 (t=1200) — also flat across the 20x main-loop-duration range. This is *not* expected:
`BroadcastSnapshot` is explicitly gated to fire roughly 60 times per second of real elapsed time
(main loop + drain combined), and the two diagnostics' real-elapsed-time totals differ by 2.73x
(5.5 s vs. 15 s) — a difference the counter does not show at all.

**This was not chased to a root cause and is not chased further here.** Diagnosing it would mean
auditing the snapshot rate gate's interaction with the drain loop's jittery, near-zero-per-iteration
`dt` values, which risks scope creep well past what answering the halo question required, in source
that remains frozen either way. It is recorded plainly: **the saving-side counters this document's
three-level breakdown depends on are not confirmed free of the same class of contamination that
corrupted the halo side.** E8's bytes/second figures are therefore unusable on both sides of the
comparison, not just the halo side — this is why the verdict above is stated as "untested" rather
than as "the halo side is untested but the saving side stands."

## The geometric estimate — labelled explicitly, not a measurement

The following is **an estimate derived from geometry, not a measurement**, and must not be read,
quoted, or cited as one: using the border-region geometry `GetOverlappedServers` was verified (round
3, above) to implement correctly — a 150×300-unit region, ~2,000 owned objects per server, an 8-unit
band, 120 Hz —

```
expected band size per server  ≈ (2000 / (150*300)) * (8*300) ≈ 106.7 objects
entries/sec, both servers      ≈ 106.7 * 120 * 2 ≈ 25,600
entry bytes/s                  ≈ 25,600 * 60 ≈ 1,536,000
packet overhead (≈6 pkts/call/server, 120 Hz, both servers) ≈ 1,440 * 52 ≈ 74,880
ESTIMATE (labelled, not measured)  ≈ 1.61 MB/s
```

This estimate assumes the timed-loop-only contribution (which this experiment could not isolate from
the drain-phase contribution) behaves as the verified-correct predicate implies, and makes no
allowance for whatever is causing the parallel `snapSent` anomaly above. It is included only to show
that the composition claim's fate is genuinely open — not settled toward "refuted," and not settled
toward "holds" either — and specifically **not** to assert that the claim holds. If it were
comparable to the (also unverified) saving figures computed in earlier rounds of this investigation,
it would sit below the smallest of them — but neither side of that comparison is currently
trustworthy, so no conclusion is drawn from it here.

## A forward-looking note on `scale-ceiling.md`, not a claim

`docs/superpowers/specs/2026-08-18-scale-ceiling.md` §6 reports interest-management snapshot
reductions as ratios between configurations: "original" 10,758,254 → peer opt-out 3,687,198 (−66%) →
+ client interest radius 50, 686,740 (−94%). Those runs used the same server binary and therefore the
same unforwardable, unthrottled drain phase. Because that document compares two *configurations that
both carried the artefact equally*, its **ratios** may well survive where E8's **absolute rates** do
not — a drain-phase contribution that is roughly constant across configurations would cancel out of
a percentage-reduction comparison in a way it cannot cancel out of a bytes/second figure. **This has
not been checked and is not being checked now** — it is flagged here as a note for whoever next works
on either document, not as a claim that `scale-ceiling.md`'s percentages are safe. Re-analysing that
document is out of scope for this task.

## Limitations

- **Single client, single workload** (`uniform`, 4,000 objects, 2 servers) in every E8-scale
  configuration measured.
- **The drain-phase finding is demonstrated at two object-count scales** (400 and 4,000) and is
  consistent at both, which is why defect 2 is reported with confidence. The *exact* timed-loop-only
  rate is not resolved, and per the discussion above, cannot be isolated with the tooling available
  in this evidence pass.
- **The `snapSent` anomaly is reported, not explained.**
- **The geometric estimate (~1.6 MB/s) is not a measurement** and is not this document's result.

## Bottom line

The composition claim — that the halo's server-to-server cost is paid for out of interest
management's server-to-client saving — is **untested** by this experiment. It is not refuted: the
25.7 MB/s and 33.9 MB/s figures that supported a "refuted" verdict in earlier rounds of this
investigation are retracted, because both were dominated by an unthrottled ~5-second drain phase
(`DistributedGameServer/ServerStarter.cpp:214`) that cannot be disabled through any supported tooling
path (`--drain-seconds` is not forwarded by `PhysicsServerMidware/ProgramStart.cpp`), confirmed by a
result — combined halo traffic varying by under 3% across a 20x range of timed-loop duration — that
has no explanation other than a fixed-duration artefact dominating the total. It is not established
either: a rough geometric estimate suggests the halo's true per-tick cost would plausibly fit under
the saving, but this is explicitly an estimate and not a measurement, and a parallel, unexplained
`snapSent` anomaly means even the saving-side figures it would be compared against are not currently
trustworthy.

**E8 is blocked on the frozen build and is left in this state.** `PhysicsServerMidware/` is frozen
this pass specifically to protect `docs/superpowers/results/2026-08-19-E5-soundness.md`'s 120 runs,
which depend on the current, unmodified drain-phase behaviour for correct handoff accounting;
breaking that freeze to fix E8 would risk the project's headline soundness result to complete a
secondary one. The three-level filtering framework and both savings framings (client-interest-only,
all-filtering) remain the right method and should be reused as-is once a clean measurement is
possible. The concrete next step is mechanical: forward `--drain-seconds` through
`PhysicsServerMidware/ProgramStart.cpp` (matching the pattern already used for a dozen other flags)
so `--drain-seconds 0` becomes reachable, then re-run `exp-bytes-paced` with it. That is a
build-phase task, not one for this evidence pass.
