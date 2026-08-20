# E8 — bytes, and the Dyconits composition claim

Date: 2026-08-19 / 2026-08-20
Runs: `runs/exp-bytes` (realtime, `-Ticks 0 -Seconds 20`) and `runs/exp-bytes-paced` (paced,
`-Ticks 1800`, no `-Seconds`), each `interestRadius` x {0, 50}, 3 repeats each, 6 runs, all
`ok: 2/2 servers`.
Commit at which the packet sizes were measured: `abf5c37` (`tools/InteractionTests/PacketSizeTests.cpp`,
same toolchain that built the servers this experiment ran). `runs/exp-bytes` was recorded against
commit `76a4794`; `runs/exp-bytes-paced` also against `76a4794`. Repo was clean before and after this
task; no file under a frozen-source path (`DistributedGameServer/`, `DistributedPhysicsManager/`,
`PhysicsServerMidware/`, `CSC8503CoreClasses/`, `CSC8503/`) was changed.

## Claim under test

Interest management makes each server send snapshots only to clients whose declared view contains
the object, cutting server-to-client traffic. The halo band (`--halo-width`) costs extra
server-to-server traffic to make cross-border collisions work. The project's concrete composition
claim against Dyconits (Donkervliet et al., ICDCS 2021) is: **the halo is paid for out of interest
management's saving** — i.e. halo bytes/s < the snapshot bytes/s saved by turning interest
management on.

**Two distinct savings, not one.** "Interest management" in this codebase is actually two separate
mechanisms that both suppress snapshot sends: peer servers declaring negative interest in each
other's objects (unrelated to any client, predates the client-facing interest radius), and the
client's own declared interest radius. `snapSupp` counts every suppressed send-decision without
distinguishing which mechanism suppressed it, so this run's own data is used to separate them (see
"Three levels", below). Both savings are reported and verdicted separately.

## A confound raised mid-task, and what checking it found

The first pass at this experiment ran only the realtime configuration (`-Ticks 0 -Seconds 20`). A
review flagged a possible confound: `PublishHaloBand()` (`DistributedGameServerManager.cpp:174`) has
no rate gate — it runs once per loop iteration unconditionally — while `BroadcastSnapshot` is
explicitly throttled to 60 Hz by `mTimeToNextPacket` (lines 176–195). The concern: if the realtime
loop spins faster than the physics substep rate, the halo would be measured at an inflated,
loop-speed-dependent rate rather than its designed per-tick cost, while snapshots would not be
similarly affected (they're gated regardless of loop speed) — an apples-to-oranges comparison biased
against the halo.

**This is checked directly, not assumed either way.** Two pieces of first-party evidence settle it
for this specific harness:

1. **The server's own log line is identical between the two configurations.** Both `runs/exp-bytes`
   and `runs/exp-bytes-paced` print `Headless pacing: REPRODUCIBLE (fixed dt 0.00833333s). Simulated
   time is decoupled from wall clock.` — because `tools/measure.ps1` passes `--fixed-step`
   unconditionally, for every run, regardless of whether the run is bounded by `-Seconds` or
   `-Ticks`. In `HeadlessRunner.cpp`, the pacing branch (`sleep_until` to `fixedDt` cadence) is gated
   on `reproducible && counting`, where `reproducible = (fixedDt > 0)` — not on which bound type is
   in effect. So **every `measure.ps1`-launched run in this evidence set, "realtime" ones included,
   already paces its outer loop to the 120 Hz physics substep rate.** The realtime/paced distinction
   in this harness controls only *when the run stops* (wall-clock seconds vs. a tick count), not how
   fast the loop spins while it runs.
2. **The tick-timing data confirms it independently.** The original `interestRadius0-r1` run's raw
   CSV: 2,395 rows spanning `time_us` 14,187,935,813 → 14,207,893,025, i.e. 19.957 s for 2,395 ticks
   = 120.03 Hz — matching the 120 Hz substep rate, not the ~1 kHz spin rate an unpaced loop would
   show.

**So the specific mechanism — the realtime run's loop outrunning the substep rate — did not occur in
this measurement**, and running the requested paced sweep (`-Ticks 1800`, below) confirms this
empirically: paced halo bytes/s came out *higher* than realtime, not ~8x lower as the loop-spin
hypothesis would predict.

**What does hold, and is recorded as a genuine finding regardless:** `PublishHaloBand()` really does
have no rate gate of its own — it is coupled 1:1 to the physics tick rate (120 Hz, twice the 60 Hz
snapshot cadence) with nothing throttling it independently, unlike `BroadcastSnapshot`. In a
deployment that does **not** pass `--fixed-step` (the timestep adapts to measured frame cost, per
`CLAUDE.md`), or on a machine where the tick rate is higher for any other reason, halo bandwidth
would scale directly and without limit with however fast the loop runs, while snapshot bandwidth
would not. That asymmetry is real, verified from source, and reported below as a defect for the
build-phase backlog — it is a different, narrower claim than "this measurement was inflated by it,"
which the data rule out.

Server code (`DistributedGameServer/`) is frozen for this evidence pass; the defect is recorded, not
fixed.

## Method

Realtime (kept from the first pass):
```
tools\run-experiments.ps1 -Name bytes -Sweep interestRadius -Values "0,50" -Repeats 3 `
    -Servers 2 -Objects 4000 -Ticks 0 -Seconds 20 -Workload uniform `
    -HaloWidth 8 -HaloLookahead 4
```

Paced (added in response to the confound check):
```
tools\run-experiments.ps1 -Name bytes-paced -Sweep interestRadius -Values "0,50" -Repeats 3 `
    -Servers 2 -Objects 4000 -Ticks 1800 -Workload uniform `
    -HaloWidth 8 -HaloLookahead 4
```
(same configuration, `-Ticks 1800` in place of `-Ticks 0 -Seconds 20`; `--fixed-step` is added by
`measure.ps1` either way, so both configurations pace identically per the finding above — the only
difference is the stopping condition.)

- `-HaloReliable` deliberately omitted in both — a real deployment does not use it, and this measures
  deployment cost, not a reproduced trajectory.
- The halo is on (width 8) at **both** interest-radius points in **both** configurations, so halo
  traffic is roughly constant within each configuration and only the snapshot side moves with
  `interestRadius`.
- Bytes/s conversion uses each run's own measured duration (realtime: `"Headless run complete after
  Ns"`; paced: `"Headless run complete after 1800 ticks (15s simulated, Ns wall)"`), not the nominal
  bound.
- **One paced run (`interestRadius50-r2`) was re-measured.** The first attempt showed one server
  taking 130.8 s wall clock against its peer's 15.04 s for the same 1,800 simulated ticks — a
  machine-contention stall, not a property of the system under test (both servers pace to the same
  fixed-dt clock in reproducible mode; an 8.7x wall-clock gap between them is not that mechanism
  behaving, it's the process being starved of CPU). That run also tripped `conservation_delta = -3`
  and `ho_parity_delta = 3`, consistent with a stalled peer causing an in-flight handoff to miss the
  run's end. It was replaced with a clean re-run (`tools\measure.ps1 ... -Tag interestRadius50-r2`,
  same parameters) that came back with both servers finishing within 0.0001 s of each other and every
  invariant `analyse.py` checks holding.
- **`ownership_gap_ticks` fails on every paced run (1,793–1,799 of 1,800 ticks).** This is a
  per-tick alignment artifact, not a real ownership gap: `analyse.py` sums `owned_objects` by raw
  tick number across servers, which assumes both servers' tick 0 lands at the same wall-clock
  instant; nothing in this run passed `--epoch-align-us` to align them, so tick *N* on server 0 and
  tick *N* on server 1 are offset by however long bootstrap differed between them, and the sum looks
  like a gap almost every tick. Conservation and handoff parity — the invariants that would actually
  catch a real ownership loss — hold cleanly on every one of the 6 paced runs (after the
  `radius50-r2` re-run). This does not affect the byte counts used below, which come from end-of-run
  `@@FINAL` totals, not the per-tick series.

## The four packet sizes (given, Task 4, commit `abf5c37`)

| Packet | payload bytes |
|---|---|
| `FullPacket` | 72 |
| `DeltaPacket` | 24 |
| `HaloObjectState` (one halo entry) | 60 |
| `HaloUpdatePacket` header | 16 |

`TRANSPORT_OVERHEAD = 36` bytes/packet (~8 ENet header + 28 UDP/IP), added to every packet, snapshot
and halo alike.

```
snapshot bytes, UPPER = count * (72 + 36) = count * 108
snapshot bytes, LOWER = count * (24 + 36) = count * 60
halo bytes             = haloSent * 16 + haloObjSent * 60 + haloSent * 36
                        = haloSent * 52 + haloObjSent * 60
bytes/s = bytes / measured_duration_s (per run, then medianed across the 3 repeats)
```

## Raw counters

Summed across both servers, from `@@FINAL role=server` lines in each run's `mid.log`. Full lines and
the computation scripts are in `.superpowers/sdd/2026-08-19-evidence-completion/logs/`.

**Realtime (`runs/exp-bytes`, `-Seconds 20`):**

| run | duration (s) | snapSent | snapSupp | haloSent | haloObjSent |
|---|---:|---:|---:|---:|---:|
| radius0-r1 | 20.0012 | 1,369,126 | 5,805,725 | 488,065 | 8,394,573 |
| radius0-r2 | 20.0025 | 1,404,764 | 5,772,212 | 489,160 | 8,512,530 |
| radius0-r3 | 20.0074 | 1,384,465 | 5,793,700 | 452,042 | 8,137,015 |
| radius50-r1 | 20.0052 | 515,726 | 6,665,539 | 443,281 | 8,196,467 |
| radius50-r2 | 20.0060 | 558,301 | 6,623,113 | 456,861 | 8,028,974 |
| radius50-r3 | 20.0057 | 523,748 | 6,657,669 | 416,694 | 8,095,940 |

**Paced (`runs/exp-bytes-paced`, `-Ticks 1800`, all runs finished at exactly 1,800 ticks, ~15.04 s
wall clock):**

| run | duration (s) | snapSent | snapSupp | haloSent | haloObjSent |
|---|---:|---:|---:|---:|---:|
| radius0-r1 | 15.0427 | 1,395,063 | 4,592,004 | 458,926 | 8,049,742 |
| radius0-r2 | 15.0410 | 1,380,910 | 4,608,470 | 440,913 | 8,118,257 |
| radius0-r3 | 15.0407 | 1,391,493 | 4,595,949 | 443,582 | 8,109,494 |
| radius50-r1 | 15.0436 | 531,041 | 5,456,364 | 471,183 | 8,794,272 |
| radius50-r2 (re-run) | 15.0435 | 553,962 | 5,435,526 | 461,647 | 8,274,753 |
| radius50-r3 | 15.0421 | 560,823 | 5,426,367 | 432,765 | 7,947,672 |

## Three levels of filtering, derived from each configuration's own data

`snapSupp` counts snapshots that *would* have been sent and were suppressed, so `snapSent + snapSupp`
at radius 0 is the total send-decision count — what an unfiltered system would have put on the wire.
Sanity check (should be radius-independent, since it's the pre-filter count): realtime 7,176,976
(radius 0) vs 7,181,414 (radius 50), 0.06% apart; paced 5,987,442 (radius 0) vs 5,987,405–5,989,488
(radius 50), <0.04% apart. Both configurations pass this check.

| level | realtime median count | realtime median UB / LB B/s | paced median count | paced median UB / LB B/s |
|---|---:|---:|---:|---:|
| unfiltered (`snapSent+snapSupp` @ radius 0) | 7,176,976 | 38,747,657.5 / 21,526,476.4 | 5,987,442 | 42,992,785.3 / 23,884,880.7 |
| peer opt-out only (`snapSent` @ radius 0) | 1,384,465 | 7,473,327.2 / 4,151,848.4 | 1,391,493 | 9,991,605.7 / 5,550,892.1 |
| peer opt-out + client interest (`snapSent` @ radius 50) | 523,748 | 2,827,440.4 / 1,570,800.2 | 553,962 | 3,976,979.9 / 2,209,433.3 |

## Halo cost: side by side

| configuration | median haloSent | median haloObjSent | entries/pkt | median halo B/s |
|---|---:|---:|---:|---:|
| realtime (`-Seconds 20`) | 454,452 | 8,166,741 | 17.97 | **25,656,033.6** |
| paced (`-Ticks 1800`) | 451,254 | 8,113,876 | 17.98 | **33,896,259.3** |

Paced is **higher**, not lower — ratio realtime/paced = 0.757x. This is the direct empirical
confirmation of the finding above: there is no loop-spin inflation in the realtime figure to correct
for in this harness. (The modest difference between the two — paced running ~32% higher — is not
investigated further here; a plausible contributor is that the paced runs are shorter overall,15 s
vs 20 s, with less relative bootstrap dead time counted out of the window, but this is not confirmed
and is left as an open observation, not a claim.)

## Two savings, verdicted separately, both configurations

Per the brief, the composition claim is asserted only if halo bytes/s is below the saving's LOWER
(least favourable) bound.

**Realtime:**
```
client-interest-only saving:  UB = 7,473,327.2 - 2,827,440.4 =  4,645,886.7   LB = 4,151,848.4 - 1,570,800.2 = 2,581,048.2
all-filtering saving:         UB = 38,747,657.5 - 2,827,440.4 = 35,920,217.0  LB = 21,526,476.4 - 1,570,800.2 = 19,955,676.1
halo cost B/s = 25,656,033.6
```

**Paced:**
```
client-interest-only saving:  UB = 9,991,605.7 - 3,976,979.9 = 6,014,625.8   LB = 5,550,892.1 - 2,209,433.3 = 3,341,458.8
all-filtering saving:         UB = 42,992,785.3 - 3,976,979.9 = 39,015,805.4  LB = 23,884,880.7 - 2,209,433.3 = 21,675,447.5
halo cost B/s = 33,896,259.3
```

| configuration | saving tested | LOWER (least favourable) | claim holds on LOWER? | ratio (LOWER) | UPPER | ratio (UPPER) |
|---|---|---:|---|---:|---:|---:|
| realtime | client-interest-only | 2,581,048.2 | NO | 9.94x | 4,645,886.7 | 5.52x |
| realtime | all-filtering | 19,955,676.1 | NO | 1.29x | 35,920,217.0 | 0.71x (holds) |
| paced | client-interest-only | 3,341,458.8 | NO | 10.14x | 6,014,625.8 | 5.64x |
| paced | all-filtering | 21,675,447.5 | NO | 1.56x | 39,015,805.4 | 0.87x (holds) |

## Verdict on the composition claim

**The verdict rests on the paced numbers**, since that is the configuration built specifically to
rule out any loop-pacing objection — even though the realtime numbers turned out, on inspection, to
already share the same pacing (see above) and so are corroborating evidence rather than a flawed
baseline.

**Client interest alone (the strong, conservative test, and the one that actually isolates the
mechanism compared to Dyconits): the claim fails, decisively, on both configurations.** Paced: halo
cost (~33.9 MB/s) exceeds the client-interest-only saving by ~10.1x on the lower (least favourable)
bound and ~5.6x on the upper bound. Realtime agrees: ~9.9x / ~5.5x. Client interest management,
evaluated on its own, does not come close to paying for the halo at this operating point, under
either configuration.

**All filtering combined, including the pre-existing peer-server opt-out: the claim still fails on
the required (lower) bound in both configurations, and the paced figure is if anything slightly
*worse* for the claim than realtime** — 1.56x over on paced LB vs 1.29x on realtime LB. Both
configurations flip to the claim *holding* on the upper bound (0.87x paced, 0.71x realtime: the
saving would exceed the halo cost if the snapshot stream were mostly full packets rather than mostly
delta packets). Since the brief requires the LOWER bound, and the LOWER bound fails in both
configurations, **the composition claim is not established under either saving, in either
configuration, at the bound the brief requires.**

**Attribution, unchanged by pacing.** Of the counted-decision reduction from unfiltered to radius 50
(paced: 5,987,442 → 1,391,493 → 553,962; realtime: 7,176,976 → 1,384,465 → 523,748), the
peer-server opt-out step accounts for the large majority of the drop in both configurations (paced:
4,595,949 of 5,433,480, ~85%; realtime: 5,792,511 of 6,653,228, ~87%), and the client's interest
radius accounts for the rest (~15% paced, ~13% realtime). The peer-server opt-out predates and is
independent of client-facing interest management and is not what the Dyconits comparison is about.
Crediting the halo against the combined saving therefore overstates what client-facing interest
management itself buys, in both configurations.

## Stated caveat (per brief)

`snapSent` does not separate full packets from delta packets — the server increments one counter for
both. The snapshot figures above are therefore genuinely a bound, not a point estimate. This
ambiguity is exactly wide enough to flip the all-filtering verdict in both configurations (fails on
LOWER, holds on UPPER) but not wide enough to touch the client-interest-only verdict in either
(fails at ~10x on LOWER and ~5.5-5.6x even on UPPER, in both configurations). Removing the ambiguity
needs a second counter (e.g. `snapFullSent` / `snapDeltaSent`) on the server side; that is a server
code change and is held for the build phase, since server sources are frozen for this evidence pass.

## The missing halo rate gate: a real finding, correctly scoped

`PublishHaloBand()` (`DistributedGameServerManager.cpp:174`, called unconditionally from
`UpdateGameServerManager` whenever the game has started) has no equivalent of `BroadcastSnapshot`'s
`mTimeToNextPacket` throttle (lines 176–195, gating snapshots to 60 Hz). The halo therefore runs at
whatever rate the physics tick advances — 120 Hz with `--fixed-step`, twice the snapshot cadence —
with no independent cap. This measurement pass could not make that manifest as a loop-speed artifact
because every run here used `--fixed-step`, which pins the tick rate (and therefore the halo publish
rate) to exactly 120 Hz regardless of wall-clock bound type. But the code path itself has no such
pin: run without `--fixed-step` (the default in a real deployment, where the timestep adapts to
measured frame cost per `CLAUDE.md`), or on a machine capable of a materially different tick rate,
and halo bandwidth would scale directly and unboundedly with the tick rate while snapshot bandwidth
would not — the asymmetry a rate-limited path is supposed to prevent. This is recorded here as a
defect for the build-phase backlog (add a rate gate to `PublishHaloBand` analogous to
`BroadcastSnapshot`'s, or document the coupling to tick rate as intentional) and is **not fixed
here** — `DistributedGameServer/` is frozen for this evidence pass.

## Limitations

- **Single client, single workload.** `--workload uniform`, 4000 objects, 2 servers, one client, in
  both configurations. Both savings are bounded by what one client's declared view can remove from a
  4000-object world; a multi-client deployment was not measured, and the peer-opt-out saving does not
  depend on client count at all (server-to-server), so a multi-client run would move only the
  client-interest component.
- **One halo configuration.** `--halo-width 8 --halo-lookahead 4` was held fixed in both
  configurations (per the brief, to isolate the interest-radius axis). The halo cost figure is
  specific to that width/lookahead pair and to this workload's object density near the border; it is
  not a general halo-cost figure.
- **The per-tick halo volume is large relative to a naive geometric estimate** (roughly
  2,000-3,000 object-entries per server per tick, out of ~2,000 owned objects, i.e. comparable to or
  exceeding the owned population, for a nominal ~8-unit-wide band on a ~300-unit border) and this was
  not fully explained during this pass — `CollectHaloPublications` (`ServerWorldManager.cpp:715`)
  reads as a straightforward narrow-band spatial query on inspection. Whatever drives the volume this
  high, it is consistent between the realtime and paced configurations (which is why the two
  configurations agree), so it is a property of the workload/implementation, not a pacing artifact —
  but the precise mechanism is left open rather than asserted.
- **One paced repeat was contention-affected and re-measured** (`interestRadius50-r2`; see Method).
  The replacement is the value used throughout; the original stalled attempt is not part of any
  figure in this document.

## Bottom line

At the tested operating point (2 servers, 4000 objects, `uniform` workload, one client,
`--halo-width 8 --halo-lookahead 4`), a mid-task review correctly flagged that `PublishHaloBand` has
no rate gate while `BroadcastSnapshot` does — a real defect, recorded above for the build-phase
backlog. Checking whether that defect had inflated this experiment's realtime measurement, however,
found that it had not: both the realtime and paced configurations pace their outer loop identically
in this harness (`measure.ps1` always passes `--fixed-step`), confirmed both from the server's own
"Headless pacing: REPRODUCIBLE" log line and from raw tick-timing data, and the paced halo figure
came back *higher* than realtime, not the ~8x lower the loop-spin hypothesis predicted.

With that checked, the verdict — now anchored on the paced configuration built to rule out the
objection, and corroborated by realtime — is unchanged in substance from the first pass: the halo's
server-to-server cost (~33.9 MB/s paced, ~25.7 MB/s realtime) exceeds **both** candidate savings at
the required least-favourable (lower) bound, in both configurations. **Client-interest-only** — the
test that actually isolates the mechanism compared to Dyconits — fails by roughly an order of
magnitude (~10.1x paced, ~9.9x realtime) and is robust to the full/delta ambiguity in `snapSent`
either way. **All-filtering** fails more narrowly (~1.6x paced, ~1.3x realtime, over the required
lower bound) and would pass on the upper bound in both configurations; since ~85-87% of that combined
saving is the pre-existing, client-independent peer-server opt-out rather than client-facing interest
management, crediting the halo against it overstates what interest management itself buys.

**The composition claim — that the halo is paid for out of interest management's saving — is not
established, under either saving framing, in either configuration, at the bound the brief requires.**
This is a refuted composition claim, checked twice (once for the pacing objection specifically) and
holding up both times.
