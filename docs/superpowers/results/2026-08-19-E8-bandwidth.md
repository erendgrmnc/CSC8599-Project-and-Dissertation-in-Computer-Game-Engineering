# E8 — bytes, and the Dyconits composition claim

Date: 2026-08-19 / 2026-08-20
Runs: `runs/exp-bytes` (realtime), `runs/exp-bytes-paced` (paced), `runs/exp-halo-diag` (small
targeted diagnostics, not part of the headline dataset). Packet sizes from commit `abf5c37`
(`tools/InteractionTests/PacketSizeTests.cpp`). Repo was clean before and after this task; no file
under a frozen-source path (`DistributedGameServer/`, `DistributedPhysicsManager/`,
`PhysicsServerMidware/`, `CSC8503CoreClasses/`, `CSC8503/`) was changed — everything below came from
reading frozen source and running the existing binaries with existing, already-supported CLI flags.

**This document went through three rounds of scrutiny, and the third overturns the first two's
numeric verdict.** All three are kept below, in order, because a results document that hides its own
false starts is worth less than one that shows them.

1. **Round 1** measured realtime only and reported the composition claim refuted (halo far more
   expensive than the interest-management saving).
2. **Round 2** checked a specific confound (the realtime loop outrunning the physics substep rate)
   by adding a paced configuration. The confound was **ruled out** — both configurations pace
   identically in this harness — and the paced numbers **confirmed** round 1's verdict, so the
   composition-claim-refuted conclusion stood, now on firmer ground.
3. **Round 3** checked a second, independent confound raised against round 2's own numbers: the
   published volume exceeds the geometric ceiling two regions can produce. That investigation found
   a real, different, previously-unidentified defect — an unthrottled ~5-second "drain" phase that
   runs after every timed loop and folds its output into the same cumulative counters the bytes/s
   figures are built from. **This invalidates the halo bytes/s figures from rounds 1 and 2 entirely.**
   The composition claim is not refuted after all — it is **untested** by this experiment as
   currently built. A rough, clearly-labelled estimate suggests it would likely hold, but that is an
   estimate, not a measurement.

**Read round 3 (below the packet sizes and the three-level filtering framework, which are still
valid) for the operative conclusion.** Rounds 1 and 2's specific bytes/s numbers for the halo are
retracted.

## Claim under test

Interest management makes each server send snapshots only to clients whose declared view contains
the object, cutting server-to-client traffic. The halo band (`--halo-width`) costs extra
server-to-server traffic to make cross-border collisions work. The claim: **the halo is paid for out
of interest management's saving** — halo bytes/s < the snapshot bytes/s saved by turning interest
management on, checked at the least favourable (lower) bound.

## The four packet sizes (given, Task 4, commit `abf5c37`) — unaffected by round 3

| Packet | payload bytes |
|---|---|
| `FullPacket` | 72 |
| `DeltaPacket` | 24 |
| `HaloObjectState` (one halo entry) | 60 |
| `HaloUpdatePacket` header | 16 |

`TRANSPORT_OVERHEAD = 36` bytes/packet. Arithmetic (unchanged, still the correct formulas — what's
wrong is the *input counters* fed into them, not the conversion):
```
snapshot bytes, UPPER = count * (72 + 36) = count * 108
snapshot bytes, LOWER = count * (24 + 36) = count * 60
halo bytes             = haloSent * 16 + haloObjSent * 60 + haloSent * 36
bytes/s = bytes / measured_duration_s
```

## Round 1 → Round 2: the pacing confound (checked, ruled out)

A review flagged that `PublishHaloBand()` (`DistributedGameServerManager.cpp:174`) has no rate gate
while `BroadcastSnapshot` is throttled to 60 Hz (lines 176–195), and hypothesised the realtime run's
main loop was spinning faster than the physics substep rate. Checked directly:

- `tools/measure.ps1` passes `--fixed-step` unconditionally, every run, regardless of `-Ticks` vs
  `-Seconds`.
- `HeadlessRunner.cpp`'s pacing branch is gated on `reproducible && counting`
  (`reproducible = fixedDt > 0`), not on bound type — so **every** `measure.ps1`-driven run paces its
  main loop to 120 Hz.
- Confirmed in two ways: both configurations' logs print the identical `Headless pacing:
  REPRODUCIBLE (fixed dt 0.00833333s)` line, and the original realtime run's raw CSV shows 2,395
  ticks over 19.957 s = 120.03 Hz, not the hypothesised ~1 kHz.

A paced sweep (`-Ticks 1800`, dropping `-Seconds`) was run to check empirically anyway. Result: paced
halo bytes/s came back **higher** than realtime (33.9 vs 25.7 MB/s at the time), not the ~8x lower
the loop-spin hypothesis predicted — confirming the main loop was not the source of the volume. **At
the time this looked like confirmation that the halo really was this expensive.** Round 3 shows why
that reading was wrong: both numbers were themselves inflated by something else entirely, common to
both configurations, which is exactly why fixing the pacing didn't move them.

## Three levels of filtering — the framework, still valid

`snapSupp` counts snapshots that would have been sent and were suppressed, so `snapSent + snapSupp`
at a given interest radius is the pre-filter send-decision count. Three levels: **unfiltered**
(`snapSent+snapSupp` @ radius 0), **peer opt-out only** (`snapSent` @ radius 0), **peer opt-out +
client interest** (`snapSent` @ radius 50). This definitional framework is unaffected by round 3's
finding — what's affected is whether the *specific numbers* measured through it can be trusted, and
per the note on the snapshot-side anomaly below, that is now also an open question, not a settled one.

## Round 3: a second confound, raised against round 2's own numbers

**The claim to check.** From `runs/exp-bytes-paced/interestRadius0-r1`, 1,800 ticks: server 0
(`objs=2011`) sent 4,270,811 halo object-states — 2,373/tick; server 1 (`objs=1989`) sent 3,778,931 —
2,099/tick. With two servers, `GetOverlappedServers` excludes the publisher's own region, so each
object has *at most one* foreign region to publish to — an absolute ceiling of 4,000
publications/tick for the *combined* world, reached only if every object were inside the band. The
combined ~4,472/tick measured is *above* that ceiling, and a width-8 band on a 300-wide world should
hold roughly 107 objects/server, not ~2,200. The neighbour's *received* shadow count (~100–127 per
`objHalo`) matches the 107 estimate closely — the receive side looks right, so the send side must be
over-publishing, over-counting, or being measured over the wrong window.

**Checked the inputs first, per the specific hypotheses raised, all read from existing logs — no
frozen file was touched:**

1. **Is `mServerBorderMap` corrupted (extra/stale/duplicate/degenerate regions)?** No.
   `runs/exp-bytes-paced/interestRadius0-r1/mid.log` shows exactly two `Received Border for server`
   lines per server — `server 0 -150/0|-150/150` and `server 1 0/150|-150/150` — a clean, correctly
   bisected 50/50 X-split. No `Registry border for server` lines followed (the idempotent registry
   path found nothing left to add), consistent with exactly two entries, never more. Both insertion
   paths (`HandleStartGameServerPacketReceived`'s legacy array loop and `ApplyServerRegistry`) guard
   with `contains()`/`find()` before inserting, so a `std::map` keyed by server id structurally
   cannot hold more than `totalServerCount` (= 2 here) entries regardless of how many times either
   path runs. No repartition ever fired (`--rebalance-interval 0`, no `--repartition-at`; confirmed
   zero `"Partition adopted"` lines in the log), so `FlushPendingPartitions` — the only other writer
   — never touched the map either.
2. **Is `mHaloWidth` really 8 at runtime?** Yes — confirmed via the forwarded `argv`:
   `--halo-width 8.000000` on every server process in every run checked.
3. **Does `GetOverlappedServers`/`CollectHaloPublications` read correctly?** On inspection, yes —
   straightforward closest-point-on-rectangle distance check, matches the geometry used elsewhere
   (the area-effect blast query). A small-object-count, small-`--halo-width` diagnostic confirms this
   empirically too: `entries per packet` at `--halo-width 1` came back at **3.0** (not the 20-entry
   cap), meaning individual publish calls really are carrying small, geometry-sized bands — if the
   predicate were broken in the direction of "matches almost everything," batches would be pinned at
   the 20-entry cap, not floating at 3.

So the three specific hypotheses in the request (corrupt border map, wrong width, broken predicate)
are all **ruled out** by direct evidence. The actual mechanism is different from all three, and
harder to see from the predicate code alone.

**What was actually found: an unthrottled ~5-second "drain" phase that runs after every timed loop
and shares the same cumulative counters.** `DistributedGameServer/ServerStarter.cpp:214–235`:

```cpp
const double drainSeconds = static_cast<double>(config.GetInt("--drain-seconds", 5));
if (drainSeconds > 0.0) {
    std::cout << "Draining for " << drainSeconds << "s so in-flight transfers land.\n";
    NCL::GameTimer drainTimer;
    double drained = 0.0;
    while (drained < drainSeconds) {
        drainTimer.Tick();
        const float dt = drainTimer.GetTimeDeltaSeconds();
        drained += dt;
        serverManager->UpdateGameServerManager(dt);   // <-- calls PublishHaloBand(), no gate, no sleep
        if (auto* worldManager = serverManager->GetServerWorldManager()) {
            worldManager->DrainScheduledArrivals();
        }
    }
}
```

This runs, by design, after `RunHeadlessLoop` returns and before the `@@FINAL` line is printed —
its whole purpose is to let in-flight handoffs land after the timed/paced loop stops, which is a
legitimate correctness feature (documented in the surrounding comment: an earlier version without it
lost 1,008 objects on a 4,000-object cluster run when a lightly-loaded server exited mid-handoff).
**But this loop has no sleep and no pacing of any kind** — it spins as fast as the CPU allows for the
full `drainSeconds` window, and on every single iteration calls `UpdateGameServerManager(dt)`, which
calls the same ungated `PublishHaloBand()` the main-loop confound investigation already cleared.
Default `drainSeconds = 5`. **`tools/measure.ps1` never passes `--drain-seconds`, and — checked
directly — `PhysicsServerMidware/ProgramStart.cpp`'s flag-forwarding block (the only place that can
deliver a game-server flag, since the midware spawns the server process) does not forward
`--drain-seconds` at all.** `grep -c drain PhysicsServerMidware/ProgramStart.cpp` returns 0. There is
therefore no way to override the 5-second default through any supported tooling path in this
evidence pass; a fix would require editing `PhysicsServerMidware/ProgramStart.cpp`, which is frozen.

**Confirmed empirically: the cumulative halo totals are essentially independent of the timed loop's
duration, which is only possible if a fixed-duration, unthrottled phase dominates them.** Three small
diagnostics (`runs/exp-halo-diag`, 2 servers, 400 objects, `--halo-width 8`, `--workload uniform`,
varying only `-Ticks`, everything else identical to the E8 config):

| ticks | main-loop duration | combined `haloObjSent` (both servers) |
|---:|---:|---:|
| 60 | 0.5 s | 4,443,170 |
| 240 | 2.0 s | 4,324,026 |
| 1,200 | 10.0 s | 4,437,202 |

Across a **20x range** of main-loop duration (0.5 s → 10 s), the total varies by less than 3% and
does **not** trend with duration at all (it is lowest at the *middle* duration) — the differences are
ordinary run-to-run noise, not a duration effect. If the timed loop's contribution were significant,
10 s of main loop should have produced roughly 20x the halo traffic of 0.5 s; it produced
statistically the same amount. This is only explained by a fixed-duration component — the 5-second
drain — dominating the total so completely that the timed loop's own contribution is undetectable
above noise at this scale.

**The same pattern holds at E8's actual scale.** The original round-1/round-2 comparison — realtime
(20.0 s main loop) vs. paced (15.04 s main loop), 4,000 objects — showed combined `haloObjSent`
medians of 8,166,741 vs. 8,113,876: a 25% difference in main-loop duration producing a 0.6%
difference in total. That was read, at the time, as *confirmation* the halo really did cost this
much regardless of pacing. It is better explained as the same drain-phase artifact appearing at both
configurations equally, because the drain window (5 s, unthrottled, identical in both) dominates
both totals almost completely.

**Estimated drain-phase rate, from the diagnostics above:** ≈4.4M combined entries / 5 s ≈
**880,000 entries/sec** (both servers combined) at 400 objects / width 8 — implying tens of thousands
of unthrottled `PublishHaloBand()` calls per second during the drain window, each publishing a
genuinely small, geometry-correct band (consistent with the entries-per-packet≈3 result at width 1).
This is not a broken predicate publishing too much *per call* — it is a correct predicate being
called an enormous, unbounded number of times because nothing paces the drain loop.

**A parallel, unresolved observation: `snapSent` shows the same flatness.** The same three-diagnostic
comparison, run on `snapSent` instead of `haloObjSent`: 139,166 (t=60) vs. 139,297 (t=1200) — also
flat across the 20x main-loop-duration range, which is *not* expected given `BroadcastSnapshot` is
explicitly gated to fire roughly 60 times per second of *real elapsed time* (main loop + drain
combined), and the two diagnostics have real-elapsed-time totals of 5.5 s and 15 s respectively — a
2.73x difference the counter does not show. This was not chased to a root cause within this pass (it
would mean auditing the snapshot gate's interaction with the drain loop's real, jittery `dt` values,
which risks going further into frozen source than this investigation needs to answer the halo
question). **It is flagged here because it means the snapshot-side counters feeding this document's
three-level breakdown and bytes/s figures may carry a related, uninvestigated issue** — the
composition-claim numbers on *both* sides of the ledger should be treated as unverified pending a
build-phase fix, not just the halo side.

## What this means for the composition claim

**Rounds 1 and 2's halo bytes/s figures (25.7–33.9 MB/s) are retracted.** They are not measurements
of the halo's per-tick or deployment cost; they are measurements of "however much an unthrottled
5-second spin loop can publish on this machine, divided by a duration that excludes that spin loop
entirely" — a quantity with no principled interpretation, and one that happened to look like
confirmation in round 2 precisely because the drain-phase artifact is present, and roughly equal, in
every configuration tested so far.

**A rough, explicitly-labelled estimate of the halo's real per-tick cost**, using the same geometry
`GetOverlappedServers` is verified to implement correctly (region 150×300 units, ~2,000 owned
objects/server, width-8 band, 120 Hz):

```
expected band size per server  ≈ (2000 / (150*300)) * (8*300) ≈ 106.7 objects
entries/sec, both servers      ≈ 106.7 * 120 * 2 ≈ 25,600
entry bytes/s                  ≈ 25,600 * 60 ≈ 1,536,000
packet overhead (≈6 pkts/call/server, both servers, 120Hz) ≈ 1,440 * 52 ≈ 74,880
ESTIMATED total                ≈ 1.61 MB/s
```

This is an **estimate from geometry, not a measurement** — it assumes the main-loop-only contribution
this experiment could not isolate behaves as the (verified-correct) predicate implies, with no
allowance for whatever is causing the parallel snapshot-side anomaly. Against the previously-computed
saving figures (themselves now flagged as unverified for the same reason), ~1.6 MB/s would sit
comfortably below even the smallest lower-bound client-interest-only saving figure computed in this
document's earlier rounds (~2.58–3.34 MB/s) — meaning **if** this estimate holds up under a clean
measurement, the composition claim would plausibly be supported, which is the opposite conclusion
from rounds 1 and 2.

**The honest verdict: the composition claim is untested by E8 as currently built, not refuted and
not confirmed.** Neither "the halo is too expensive" (rounds 1–2) nor "the halo comfortably fits" (the
geometric estimate above) can be asserted as a measured result. Both the halo-side and,
provisionally, the snapshot-side cumulative counters are contaminated by an unthrottled phase whose
duration is fixed and whose contribution cannot currently be isolated from the timed loop's own
contribution through any supported tooling path.

## What a valid measurement needs (build-phase backlog, not fixed here)

1. **Forward `--drain-seconds` through the midware.** `PhysicsServerMidware/ProgramStart.cpp`'s
   flag-forwarding block (lines ~44–91) already forwards over a dozen game-server flags by the same
   pattern (`if (config.Has("--X")) serverExtraArgs += ...`); `--drain-seconds` is conspicuously
   absent despite being a real, already-implemented `ServerStarter.cpp` flag. Adding it would let a
   measurement run pass `--drain-seconds 0` and get a clean, timed-loop-only figure. This is the
   most direct fix and the one this investigation would use first.
2. **Or: give `PublishHaloBand` a rate gate**, matching `BroadcastSnapshot`'s `mTimeToNextPacket`
   pattern, so it cannot run away regardless of which loop — main or drain — calls it.
3. **Separately, diagnose why `snapSent` does not scale with real elapsed time across the drain
   boundary**, since the answer may be the same root cause (the drain loop's jittery, near-zero
   per-iteration `dt` interacting badly with a rate-gate check written assuming reasonably-sized `dt`
   values) or a different one.

None of these are implemented here — `DistributedGameServer/` and `PhysicsServerMidware/` are frozen
for this evidence pass. Once (1) or (2) lands, re-running exactly the `exp-bytes-paced` sweep with
`--drain-seconds 0` (once forwardable) would give the first trustworthy halo bytes/s figure this
investigation has produced.

## Stated caveat, still true regardless of round 3

`snapSent` does not separate full packets from delta packets, so even a corrected measurement would
still need the UPPER/LOWER bound treatment described earlier in this document. That ambiguity is
unrelated to, and not resolved by, the drain-phase finding.

## Limitations

- **Single client, single workload**, `uniform`, 4,000 objects, 2 servers, in every configuration
  measured. Not tested at other scales.
- **The drain-phase finding is demonstrated at two object-count scales (400 and 4,000)** and is
  consistent at both, which is why it is reported with confidence; the *exact* main-loop-only rate
  is not, and — per the discussion above — cannot currently be isolated with the tooling available in
  this evidence pass.
- **The snapshot-side anomaly is reported, not explained.** It was not investigated past confirming
  it exists, because doing so risked scope creep into frozen source well beyond what answering the
  halo question required.
- **The geometric estimate (~1.6 MB/s) is not a measurement.** It should not be cited as this
  document's result; it is included only to show that the composition claim's fate is genuinely
  undetermined, not to replace one unverified number with another.

## Bottom line

This document went through three rounds. Round 1 measured the halo as far more expensive than
interest management's saving and reported the composition claim refuted. Round 2 checked a specific
confound (main-loop pacing), ruled it out with direct evidence, and — because the paced figures
agreed with the realtime ones — took that agreement as confirmation the refutation was solid. Round 3
checked a second confound raised against round 2's own arithmetic (the published volume exceeds what
two regions can physically produce), ruled out the three specific hypotheses offered for it (a
corrupt border map, a wrong halo width, a broken distance predicate — all verified clean), and
instead found a different, real, and previously unidentified defect: an unthrottled ~5-second drain
phase, present after every timed loop in this harness with no way to disable it through any supported
flag, that folds its own uncapped `PublishHaloBand()` output into the same cumulative counters this
document's entire bytes/s methodology is built on — which explains, retroactively, why the paced and
realtime figures agreed so closely in round 2: both were dominated by the same fixed-duration
artifact, not by the mechanism being measured.

**The composition claim is therefore untested, not refuted.** Rounds 1 and 2's numeric verdict is
withdrawn. A rough geometric estimate (~1.6 MB/s) suggests the halo's true per-tick cost would likely
fit comfortably under the smallest saving figure computed here, which would support the claim rather
than refute it — but this is stated as an estimate, explicitly not a result, and a parallel,
unexplained anomaly in the snapshot-side counters means even the saving figures it would be compared
against are not currently trustworthy either. A valid answer needs a build-phase fix — most directly,
forwarding `--drain-seconds` through the midware so a clean `--drain-seconds 0` run becomes possible
— which is out of scope for this frozen-source evidence pass and is recorded here as the concrete
next step.
