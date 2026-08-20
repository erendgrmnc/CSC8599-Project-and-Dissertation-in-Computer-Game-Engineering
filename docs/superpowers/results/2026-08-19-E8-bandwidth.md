# E8 — bytes, and the Dyconits composition claim

Date: 2026-08-19 / 2026-08-20
Runs: `runs/exp-bytes` (`interestRadius` x {0, 50}, 3 repeats each, 6 runs, all `ok: 2/2 servers`)
Commit at which the packet sizes were measured: `abf5c37` (`tools/InteractionTests/PacketSizeTests.cpp`,
same toolchain that built the servers this experiment ran). Repo was clean at the start and end of
this task; the sweep itself changed nothing under a frozen-source path.

## Claim under test

Interest management makes each server send snapshots only to clients whose declared view contains
the object, cutting server-to-client traffic. The halo band (`--halo-width`) costs extra
server-to-server traffic to make cross-border collisions work. The project's concrete composition
claim against Dyconits (Donkervliet et al., ICDCS 2021) is: **the halo is paid for out of interest
management's saving** — i.e. halo bytes/s < the snapshot bytes/s saved by turning interest
management on. This was previously argued qualitatively (packet counts, no byte conversion). This
experiment converts it to bytes/s and checks the inequality at the least favourable reading.

**Two distinct savings, not one.** "Interest management" in this codebase is actually two separate
mechanisms that both suppress snapshot sends: peer servers declaring negative interest in each
other's objects (unrelated to any client, predates the client-facing interest radius), and the
client's own declared interest radius. `snapSupp` counts every suppressed send-decision without
distinguishing which mechanism suppressed it, so this run's own data — not an older run — is used to
separate them (see "Three levels", below). Both are reported and verdicted separately; neither is
picked over the other.

## Method

```
tools\run-experiments.ps1 -Name bytes -Sweep interestRadius -Values "0,50" -Repeats 3 `
    -Servers 2 -Objects 4000 -Ticks 0 -Seconds 20 -Workload uniform `
    -HaloWidth 8 -HaloLookahead 4
```

- **Realtime, not paced** (`-Ticks 0 -Seconds 20`): bandwidth is a rate, so it has to be measured
  against wall-clock time, not a fixed tick budget. `analyse.py` correctly skips the per-tick
  ownership-gap check on realtime runs (it compares ticks across servers that are not the same
  simulated instant in realtime mode) — its absence from this run's invariant report is expected,
  not a gap in coverage. All 6 runs pass every invariant `analyse.py` does check (conservation,
  handoff parity, command accounting, no failures, no resurrections).
- `-HaloReliable` deliberately omitted — a real deployment does not use it, and this measures
  deployment cost, not a reproduced trajectory.
- The halo is on (width 8) at **both** points of the sweep, so halo traffic is constant across it
  and only the snapshot side should move with `interestRadius`.
- Actual measured run length (`"Headless run complete after Ns"` from `mid.log`, both servers
  averaged per run) was used for the bytes/s conversion, not the nominal 20 s. Measured durations
  ranged 20.0011-20.0083 s, i.e. within 0.04% of nominal — using the actual figure changes nothing
  materially, but it's what was used.

## The four packet sizes (given, Task 4, commit `abf5c37`)

| Packet | payload bytes |
|---|---|
| `FullPacket` | 72 |
| `DeltaPacket` | 24 |
| `HaloObjectState` (one halo entry) | 60 |
| `HaloUpdatePacket` header | 16 |

`TRANSPORT_OVERHEAD = 36` bytes/packet (~8 ENet header + 28 UDP/IP), added to every packet,
snapshot and halo alike, per the brief.

## Raw counters

Summed across both servers, from `@@FINAL role=server` lines in each run's `mid.log`
(`.superpowers/sdd/2026-08-19-evidence-completion/logs/` holds the sweep log and the two computation
scripts used below; raw per-server lines are quoted in the task report).

| run | duration (s, measured) | snapSent | snapSupp | haloSent | haloObjSent | entries/pkt |
|---|---:|---:|---:|---:|---:|---:|
| radius0-r1 | 20.0012 | 1,369,126 | 5,805,725 | 488,065 | 8,394,573 | 17.20 |
| radius0-r2 | 20.0025 | 1,404,764 | 5,772,212 | 489,160 | 8,512,530 | 17.40 |
| radius0-r3 | 20.0074 | 1,384,465 | 5,793,700 | 452,042 | 8,137,015 | 18.00 |
| radius50-r1 | 20.0052 | 515,726 | 6,665,539 | 443,281 | 8,196,467 | 18.49 |
| radius50-r2 | 20.0060 | 558,301 | 6,623,113 | 456,861 | 8,028,974 | 17.57 |
| radius50-r3 | 20.0057 | 523,748 | 6,657,669 | 416,694 | 8,095,940 | 19.43 |

These match `analyse.py`'s `summary.csv` (`snap_sent`, `snap_suppressed` columns) exactly. Halo is
constant across the sweep by design (width 8 at both points); the small run-to-run spread in
`haloSent`/`haloObjSent` (roughly 416k-489k packets, 8.03M-8.51M entries) is repeat variance around
that constant, not a trend. Median halo counters across all 6 runs: `haloSent` = 454,452,
`haloObjSent` = 8,166,741, entries/packet = 17.97 (halo packets run consistently near the
`HaloUpdatePacket` 20-entry batch cap).

## Three levels of filtering, derived from this run's own data

`snapSupp` counts snapshots that *would* have been sent and were suppressed — so `snapSent +
snapSupp` at a given interest radius is the total number of send *decisions*, i.e. exactly what an
unfiltered system would have put on the wire. Sanity check: this total should not depend on the
client's declared interest radius at all, since it's the pre-filter decision count — and it doesn't:
7,176,976 (median, radius 0) vs 7,181,414 (median, radius 50), a 0.06% difference, confirming
`snapSent + snapSupp` is a stable, radius-independent baseline in this dataset. That gives three
self-consistent levels, all from `runs/exp-bytes`, medians across the 3 repeats, summed across
servers:

| level | what it represents | median count | median UB bytes/s | median LB bytes/s |
|---|---|---:|---:|---:|
| unfiltered | `snapSent + snapSupp` at radius 0 — every send decision, before either suppression mechanism | 7,176,976 | 38,747,657.5 | 21,526,476.4 |
| peer opt-out only | `snapSent` at radius 0 — client interest disabled, only inter-server negative interest active | 1,384,465 | 7,473,327.2 | 4,151,848.4 |
| peer opt-out + client interest | `snapSent` at radius 50 — both mechanisms active | 523,748 | 2,827,440.4 | 1,570,800.2 |

(UB = `count * (72+36) / duration`, LB = `count * (24+36) / duration`, both per the arithmetic
below, medians of the per-run bytes/s figures.)

Context, clearly not part of this dataset: an earlier build (`docs/superpowers/specs/2026-08-18-scale-ceiling.md`,
before several later changes, 30 s runs) measured "original" 10,758,254 object-snapshots, then peer
opt-out 3,687,198 (-66%), then + client interest radius 50, 686,740 (-94%). The shape (large
baseline, big first-mechanism drop, further second-mechanism drop) is consistent with this run's own
numbers, but the absolute figures are from a different build and are not mixed into the table above.

## Bytes/s arithmetic

```
FULL = 72, DELTA = 24, HALO_ENTRY = 60, HALO_HDR = 16, OVERHEAD = 36

snapshot bytes, UPPER = count * (72 + 36) = count * 108
snapshot bytes, LOWER = count * (24 + 36) = count * 60
halo bytes             = haloSent * 16 + haloObjSent * 60 + haloSent * 36
                        = haloSent * 52 + haloObjSent * 60
bytes/s = bytes / measured_duration_s (per run, then medianed across repeats)
```

Halo cost (server-to-server, constant across the sweep): median 25,656,034 B/s (~25.66 MB/s) across
all 6 runs (25.36-26.81 MB/s per individual run; no trend with radius, as expected since halo width
was held at 8 throughout).

## Two savings, verdicted separately

The halo is a server-to-server cost. There are two different, legitimate arguments for what it
should be weighed against, and they answer different questions:

- **Client-interest-only saving** (peer opt-out only -> peer opt-out + client interest, i.e. radius
  0 -> radius 50 `snapSent`). This isolates the saving attributable to the client's declared
  interest radius by itself — the harder, more conservative test, since it holds the peer-opt-out
  mechanism fixed on both sides.
- **All-filtering saving** (unfiltered -> peer opt-out + client interest, i.e. `snapSent+snapSupp`
  at radius 0 -> `snapSent` at radius 50). This is the saving from every suppression mechanism
  combined, including the peer-server opt-out that predates and is independent of the client
  interest radius.

Per the brief, the composition claim is asserted only if halo bytes/s is below the saving's LOWER
(least favourable) bound.

```
halo cost B/s (median, all 6 runs) = 25,656,033.6

client-interest-only saving:
  UPPER = 7,473,327.2 - 2,827,440.4 =  4,645,886.7
  LOWER = 4,151,848.4 - 1,570,800.2 =  2,581,048.2

all-filtering saving:
  UPPER = 38,747,657.5 - 2,827,440.4 = 35,920,217.0
  LOWER = 21,526,476.4 - 1,570,800.2 = 19,955,676.1
```

| saving tested | LOWER bound (least favourable) | claim holds on LOWER? | halo/saving ratio (LOWER) | UPPER bound | halo/saving ratio (UPPER) |
|---|---:|---|---:|---:|---:|
| client-interest-only | 2,581,048.2 B/s | NO | 9.94x | 4,645,886.7 B/s | 5.52x |
| all-filtering | 19,955,676.1 B/s | NO | 1.29x | 35,920,217.0 B/s | 0.71x (holds) |

## Verdict on the composition claim

**Client interest alone (the strong, conservative test): the claim fails, decisively.** Halo cost
(~25.66 MB/s) exceeds the client-interest-only saving by ~9.9x on the least favourable (lower)
snapshot bound and ~5.5x on the upper bound. Client interest management, evaluated on its own, does
not come close to paying for the halo at this operating point.

**All filtering combined, including the pre-existing peer-server opt-out: the claim still fails on
the required (lower) bound, but only narrowly — 1.29x, not an order of magnitude — and it flips to
holding on the upper bound** (0.71x: the saving would exceed the halo cost by roughly 40% if the
snapshot stream were mostly full packets rather than mostly delta packets). Since the brief requires
the LOWER bound for the claim to be asserted, and the LOWER bound fails here too, the composition
claim is not established under either saving, at the least favourable reading required. But the
margin is materially different: client-interest-only misses by an order of magnitude, while
all-filtering misses by 29% and would pass on the upper bound — i.e. the all-filtering case sits
right at the boundary the full/delta ambiguity in `snapSent` could plausibly resolve either way,
whereas the client-interest-only case does not.

**Attribution matters here.** Of the 6,653,228 counted-decision reduction from unfiltered to radius
50 (unfiltered 7,176,976 -> peer-opt-out-only 1,384,465 -> radius-50 523,748), the peer-server
opt-out step accounts for 5,792,511 of that drop (about 87%), and the client's interest radius
accounts for the remaining 860,717 (about 13%). The peer-server opt-out is a separate mechanism from
client-side interest management, existed before the interest radius was added, and is not what the
Dyconits comparison is about — Dyconits is a client-facing consistency/interest technique. Crediting
the halo against the combined saving therefore mixes in a saving that has nothing to do with the
mechanism being compared to Dyconits. The client-interest-only verdict is the one that actually
answers the stated claim; the all-filtering verdict is reported alongside it, plainly labelled, so
neither is hidden.

## Stated caveat (per brief)

`snapSent` does not separate full packets from delta packets — the server increments one counter for
both. The snapshot figures above are therefore genuinely a bound, not a point estimate: the true
bytes/s lies somewhere between the LOWER and UPPER columns depending on the actual, unmeasured
full:delta mix in each run (nominally 1:5, but not verified by this counter). This ambiguity is
exactly wide enough to flip the all-filtering verdict (fails on LOWER at 1.29x, holds on UPPER at
0.71x) but not wide enough to touch the client-interest-only verdict (fails at 9.94x on LOWER and
5.52x even on UPPER). Removing the ambiguity needs a second counter (e.g. `snapFullSent` /
`snapDeltaSent`) on the server side; that is a server code change and is held for the build phase,
since server sources are frozen for this evidence pass.

## Limitations

- **Single client, single workload.** `--workload uniform`, 4000 objects, 2 servers, one client.
  Both savings are bounded by what one client's declared view can remove from a 4000-object world;
  a multi-client deployment was not measured, and the peer-opt-out saving does not depend on client
  count at all (it is server-to-server), so a multi-client run would move only the client-interest
  component.
- **One halo configuration.** `--halo-width 8 --halo-lookahead 4` was held fixed (per the brief, to
  isolate the interest-radius axis). The halo cost figure is specific to that width/lookahead pair
  and to this workload's object density near the border; it is not a general halo-cost figure.
- **Realtime run, not paced.** Correct for a bandwidth measurement (see Method), but it means exact
  tick counts and handoff timing are not bit-reproducible run to run, consistent with every other
  realtime run in this evidence set.

## Bottom line

At the tested operating point (2 servers, 4000 objects, `uniform` workload, one client,
`--halo-width 8 --halo-lookahead 4`), the halo's server-to-server cost (~25.66 MB/s) exceeds both
candidate savings at the required least-favourable (lower) bound: client-interest-only by ~9.9x
(~2.58 MB/s saving) and all-filtering combined by ~1.29x (~19.96 MB/s saving). The composition claim
— that the halo is paid for out of interest management's saving — is not established under either
framing, at the bound the brief requires.

The two verdicts are not equally strong evidence, though. The client-interest-only test — the one
that actually isolates the mechanism being compared to Dyconits — fails by an order of magnitude and
is robust to the full/delta ambiguity in `snapSent` either way. The all-filtering test fails much
more narrowly (29% over on the lower bound, and would pass on the upper bound), and roughly 87% of
that combined saving comes from the pre-existing peer-server opt-out rather than from client-facing
interest management, so crediting the halo against it overstates what interest management itself
buys. Read together: the composition claim as stated (interest management pays for the halo) is
refuted for interest management proper; it is unresolved-but-close for "everything the codebase
calls filtering, combined," and resolving that close case needs the full/delta split this pass
deliberately deferred.
