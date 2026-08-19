# E8 — bytes, and the Dyconits composition claim

Date: 2026-08-19
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
  ranged 20.0011–20.0083 s, i.e. within 0.04% of nominal — using the actual figure changes nothing
  materially, but it's what was used.

## The four packet sizes (given, Task 4, commit `abf5c37`)

| Packet | payload bytes |
|---|---|
| `FullPacket` | 72 |
| `DeltaPacket` | 24 |
| `HaloObjectState` (one halo entry) | 60 |
| `HaloUpdatePacket` header | 16 |

`TRANSPORT_OVERHEAD = 36` bytes/packet (~8 ENet header + 28 UDP/IP), added to **every** packet,
snapshot and halo alike, per the brief.

## Raw counters

Summed across both servers, from `@@FINAL role=server` lines in each run's `mid.log`
(`.superpowers/sdd/2026-08-19-evidence-completion/logs/` holds the sweep log and analysis script
used below; raw per-server lines are quoted in the task report).

| run | duration (s, measured) | snapSent | haloSent | haloObjSent | entries/pkt |
|---|---:|---:|---:|---:|---:|
| radius0-r1 | 20.0012 | 1,369,126 | 488,065 | 8,394,573 | 17.20 |
| radius0-r2 | 20.0025 | 1,404,764 | 489,160 | 8,512,530 | 17.40 |
| radius0-r3 | 20.0074 | 1,384,465 | 452,042 | 8,137,015 | 18.00 |
| radius50-r1 | 20.0052 | 515,726 | 443,281 | 8,196,467 | 18.49 |
| radius50-r2 | 20.0060 | 558,301 | 456,861 | 8,028,974 | 17.57 |
| radius50-r3 | 20.0057 | 523,748 | 416,694 | 8,095,940 | 19.43 |

`snapSent` is confirmed the same value `analyse.py`'s `summary.csv` reports per run (it sums
`snapSent` across servers before writing it into every server row, so the two per-server rows in
`summary.csv` for a given run carry an identical, already-summed figure — read once per run, not
per server-row, or the totals below would double).

**Medians across repeats** (the quantities used for the bytes/s figures):

| point | median snapSent | median haloSent | median haloObjSent |
|---|---:|---:|---:|
| radius 0 | 1,384,465 | 488,065 | 8,394,573 |
| radius 50 | 523,748 | 443,281 | 8,096,940* |

(*8,095,940, the true median of {8,196,467, 8,028,974, 8,095,940}.)

Halo is constant across the sweep by design (width 8 at both points); the small run-to-run spread
in `haloSent`/`haloObjSent` (≈416k–489k packets, ≈8.03M–8.51M entries) is repeat variance around
that constant, not a trend. Median halo counters across all 6 runs: `haloSent` = 454,452,
`haloObjSent` = 8,166,741, entries/packet = 17.97 (halo packets run consistently near the
`HaloUpdatePacket` 20-entry batch cap).

**`snap_sent` saving, radius 0 -> 50:** median 1,384,465 -> 523,748, a drop of 860,717 packets
(62.2%).

**Caveat on what that saving represents.** At radius 0 the run already carries a large
`snapSupp` count (~2.9M suppressions per server) that has nothing to do with the client's declared
interest radius: peer servers already decline snapshots for objects outside their own area of
responsibility, independent of any client-side filtering. That server-to-server suppression is
present, and identical in kind, at **both** ends of this sweep — it is baked into both the radius-0
and radius-50 measurements equally. The 62.2% figure above is therefore the **marginal saving from
the client's declared interest radius alone**, on top of whatever suppression already existed at
radius 0. It is not a comparison against a fully unfiltered baseline, and should not be read as one.

## Bytes/s, bounded

```
snapshot bytes, UPPER = snapSent * (72 + 36) = snapSent * 108
snapshot bytes, LOWER = snapSent * (24 + 36) = snapSent * 60
halo bytes            = haloSent * 16 + haloObjSent * 60 + haloSent * 36
                       = haloSent * 52 + haloObjSent * 60
```
(equivalent to `haloSent * (16 + 60*entries_per_packet + 36)`, computed exactly rather than through
the rounded entries/packet ratio.)

Converted to bytes/second using each run's own measured duration, then medianed across the 3
repeats per point:

| point | snapshot UPPER (B/s) | snapshot LOWER (B/s) | halo (B/s) |
|---|---:|---:|---:|
| radius 0 | 7,473,327 | 4,151,848 | 26,451,167 |
| radius 50 | 2,827,440 | 1,570,800 | 25,364,059 |
| **median across all 6 runs (halo only, since halo is constant)** | – | – | **25,656,034** |

Per-run figures (all 6, medians above are computed from this table):

| run | snapshot UPPER B/s | snapshot LOWER B/s | halo B/s |
|---|---:|---:|---:|
| radius0-r1 | 7,392,855 | 4,107,142 | 26,451,167 |
| radius0-r2 | 7,584,778 | 4,213,765 | 26,806,055 |
| radius0-r3 | 7,473,327 | 4,151,848 | 25,576,827 |
| radius50-r1 | 2,784,197 | 1,546,776 | 25,735,240 |
| radius50-r2 | 3,013,921 | 1,674,401 | 25,267,180 |
| radius50-r3 | 2,827,440 | 1,570,800 | 25,364,059 |

**Snapshot saving (interest management's benefit), radius 0 -> 50:**

- Upper bound: 7,473,327 - 2,827,440 = **4,645,887 B/s**
- Lower bound (least favourable): 4,151,848 - 1,570,800 = **2,581,048 B/s**

**Halo cost (server<->server, constant across the sweep):** median **25,656,034 B/s** (~25.66 MB/s),
consistent across both sweep points (25.36–26.81 MB/s per individual run; no trend with radius, as
expected since halo width was held at 8 throughout).

## Verdict on the composition claim

The composition claim requires halo bytes/s < snapshot saving bytes/s, checked at the **least
favourable reading**: halo cost as computed above, against the saving computed with the **lower**
snapshot bound.

```
halo cost         : ~25,656,034 B/s
saving (LOWER)     : ~2,581,048 B/s
saving (UPPER)     : ~4,645,887 B/s
```

Halo cost exceeds the snapshot saving at **both** bounds, by roughly 5.5x on the upper bound and
roughly **9.9x** on the lower, least-favourable bound.

**The composition claim is not established. It is refuted at this operating point.** The halo does
not merely fail to be "paid for" by interest management's saving — under this workload
(`--workload uniform`, 4000 objects, 2 servers, `--halo-width 8 --halo-lookahead 4`) it costs an
order of magnitude more than interest management saves, at either snapshot bound. This is the least
favourable reading requested by the brief, and it is decisive in that direction: the gap (5.5x-9.9x)
is far larger than the spread between the upper and lower snapshot bounds themselves, so the
ambiguity in `snapSent`'s full/delta mix does not affect which way this verdict comes out.

The likely structural reason, stated for context rather than as a new claim needing its own
experiment: the measured run has exactly **one client**, so interest management's saving is bounded
by what a single declared view can remove from a 4000-object world. The halo cost, by contrast, is
driven by every object within 8 units of the shared border being republished by each server to its
neighbour on **every tick** (60 Hz), independent of how many clients exist or what any of them
declared interest in. Server-to-server cost scales with world density and border geometry; the
measured saving here scales with client count and view size. A workload with more concurrent clients
each declaring a narrow interest radius could shift this balance; this experiment did not test that
axis and does not claim to.

## Stated caveat (per brief)

`snapSent` does not separate full packets from delta packets — the server increments one counter
for both. The snapshot figures above are therefore genuinely a **bound**, not a point estimate:
the true bytes/s lies somewhere between the LOWER and UPPER columns depending on the actual,
unmeasured full:delta mix in each run (nominally 1:5, but not verified by this counter).
Removing that ambiguity needs a second counter (e.g. `snapFullSent` / `snapDeltaSent`) on the
server side; that is a server code change and is held for the build phase, since server sources are
frozen for this evidence pass. It does not change the verdict here, because the halo cost clears
both bounds by a wide margin.

## Limitations

- **Single client, single workload.** `--workload uniform`, 4000 objects, 2 servers, one client.
  The saving side of the comparison is bounded by one client's declared view; a multi-client
  deployment was not measured.
- **One halo configuration.** `--halo-width 8 --halo-lookahead 4` was held fixed (per the brief, to
  isolate the interest-radius axis). The halo cost figure is specific to that width/lookahead pair
  and to this workload's object density near the border; it is not a general halo-cost figure.
- **Realtime run, not paced.** Correct for a bandwidth measurement (see Method), but it means
  exact tick counts and handoff timing are not bit-reproducible run to run, consistent with every
  other realtime run in this evidence set.

## Bottom line

At the tested operating point (2 servers, 4000 objects, `uniform` workload, one client,
`--halo-width 8 --halo-lookahead 4`), the halo's server-to-server cost (~25.7 MB/s) is roughly
5.5x to 9.9x larger than interest management's server-to-client saving (~4.65 MB/s upper bound,
~2.58 MB/s lower/least-favourable bound), both figures computed with the required 36-byte
transport overhead included. **The composition claim — that the halo is paid for out of interest
management's saving — does not hold at the least favourable reading, and does not hold at the
favourable one either.** This refutes the qualitative version of the claim rather than confirming
it; the likely reason is that this run measured one client (bounding the saving) against a
per-tick, per-border-object halo republish cost (unbounded by client count), and a workload with
substantially more concurrent clients was not tested.
