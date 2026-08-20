# E8 — bytes, and the Dyconits composition claim

Date: 2026-08-19 (blocked) / 2026-08-20 (measured)
Runs: `runs/exp-bytes-clean` (the measurement below). Superseded: `runs/exp-bytes`,
`runs/exp-bytes-paced`, `runs/exp-halo-diag` (contaminated — see "Retraction").
Packet sizes from `tools/InteractionTests/PacketSizeTests.cpp`.

## Verdict

> **The composition claim — "the halo is paid for out of interest management's saving" — holds at one
> client for interest radii 25 and 50 under a per-datagram cost model, and is bracketed rather than
> settled under a payload-only model. It holds at every radius tested once two or more clients are
> connected.** The halo's cost is **~1.6 MB/s**, flat across interest radius as it must be
> (server-to-server traffic cannot depend on what a client asked for). What decides the comparison is
> not the halo but how snapshot datagrams are costed, because interest management removes *many tiny
> packets* while the halo adds *few large ones*.

## Configuration

2 servers, 4,000 objects, `uniform`, 1 client, 20 s realtime, 3 repeats, seed 42, world
`-150,150,-150,150`, `--halo-width 8 --halo-lookahead 4` (floor at L=4 is 6.0, so the band is sound),
halo **unreliable** — the deployment-realistic choice; `--halo-reliable` exists for reproducibility and
would only raise the halo's cost. `--drain-seconds 0`, which is what made this measurable at all.

Realtime rather than paced, because the claim is a *rate*.

## The measurement

Bytes are derived from counters and measured `sizeof`, never hand-added struct members:
`full=72, delta=24, haloEntry=60, haloHeader=16`. The snapshot schedule is 1 full : 5 delta
(`mPacketsToSnapshot` counts 5 down to -1), so the average snapshot payload is
`(72 + 5*24)/6 = 32 B`. Halo batches measured **18.1–19.3 entries per packet** against a cap of 20.

| interest radius | snapshot B/s | halo B/s | saving vs radius 0 | halo / saving |
|---|---|---|---|---|
| 0 (everything) | 4,630,270 | 1,615,376 | — | — |
| 25 | 1,477,392 | 1,672,717 | 3,152,878 | **0.531** |
| 50 | 1,638,533 | 1,607,167 | 2,991,737 | **0.537** |
| 100 | 3,039,035 | 1,634,495 | 1,591,235 | **1.027** |

Medians of 3 repeats, costing every datagram at payload + 36 B transport (IPv4 20 + UDP 8 + ENet
header and command ~8). Snapshot volume is monotone in radius (25 < 50 < 100 < unlimited), and halo
volume is flat within 4% across all four radii — the two sanity checks this experiment has.

**Cross-check against E3.** The radius-0 figure implies ~1.36 M object-snapshots over 20 s; E3
independently recorded 1,317,106 for the same object count and duration. Within 3%, from a different
experiment and a different build.

## The result turns on per-datagram overhead, and that is a real uncertainty

| | radius 25 | radius 50 | radius 100 |
|---|---|---|---|
| payload + 36 B per datagram | 0.531 ✅ | 0.537 ✅ | 1.027 ~ |
| payload only | 1.092 ❌ | 1.105 ❌ | 2.116 ❌ |

The two models disagree on the verdict, and the reason is structural: a delta snapshot is 24 B of
payload, so 36 B of headers nearly triples it, while a halo packet carries ~18 entries (~1,100 B) and
is barely affected. **Interest management removes exactly the packets that overhead hurts most.**

Neither bound is obviously correct. Against the per-datagram model: **ENet coalesces outgoing commands
into MTU-sized datagrams**, so a tick's worth of small snapshots may share far fewer datagrams than
there are snapshots, pushing the true cost toward the payload-only figure. In favour of it: 36 B is
itself conservative (ENet's own per-command header pushes the real figure nearer 40–44), and
coalescing is bounded by how many snapshots are actually in flight per service call, which is not
measured here. **The honest statement is that the true value lies between these two rows, and locating
it requires measuring datagrams rather than packets** — ENet exposes `totalSentData` on the host, which
would settle it and needs a small code change to surface.

## Where it is not marginal: client count

Snapshot traffic is counted **per object per client** (`mSnapshotsSent += targets.size()` on the
broadcast path, `++mSnapshotsSent` per peer on the filtered path), so both the snapshot cost and the
interest-management saving scale linearly with connected clients. The halo is server-to-server and does
not scale with clients at all.

This run has **one** client, which is the least favourable case the claim can be put in. At `C` clients
the ratio is `halo / (C * saving)`:

| clients | radius 25 (payload-only bound) | radius 100 (payload-only bound) |
|---|---|---|
| 1 | 1.09 ❌ | 2.12 ❌ |
| 2 | 0.55 ✅ | 1.06 ~ |
| 3 | 0.36 ✅ | 0.71 ✅ |

Under the per-datagram model every cell at 2+ clients holds comfortably. **This is an analytical
extrapolation from how the counters increment, not a measurement** — `measure.ps1` starts a single
client (`-LateClientAfter` adds a second, but joins it mid-run), so measuring it needs multi-client
support in the harness. Recorded as a backlog item rather than claimed.

## What this does and does not establish

- **Established:** the halo costs ~1.6 MB/s server-to-server at 4,000 objects on a 2-server partition,
  independent of client interest; and interest management's saving at one client is 1.6–3.2 MB/s
  depending on radius.
- **Established:** the composition claim holds under the per-datagram model at radii 25 and 50, and
  under either model at 2+ clients for those radii.
- **Not established:** the single-client, large-radius (100) case, which is marginal under one model
  and fails under the other.
- **Not measured:** actual datagrams on the wire. This is the one remaining gap and it is small.

The qualitative claim the roadmap wanted — that the halo is affordable out of interest management's
saving — survives, but with a sharper mechanism than "the halo is cheap": **the halo is affordable
because it batches and snapshots do not.** That is a design property worth stating rather than a
measurement artefact to hide.

## Retraction (2026-08-19 investigation)

This document previously reported the halo's cost as **25,656,033.6 B/s (~25.7 MB/s)** realtime and
**33,896,259.3 B/s (~33.9 MB/s)** paced, and concluded the composition claim was refuted by roughly an
order of magnitude. **Both figures are retracted** and are wrong by a factor of ~16 against the
measurement above. They measured an unthrottled spin loop, divided by a duration that excluded it. The
numbers are kept visible here, with the retraction attached, because they were quoted in status
reports before the defect was found.

Two defects caused it, both now fixed:

- **`PublishHaloBand()` had no rate gate** — published once per loop iteration rather than once per
  tick. Fixed; see `docs/EVALUATION.md` §4.1, which also records that it was corrupting invariant I8,
  not merely inflating bandwidth.
- **`--drain-seconds` was parsed but never forwarded**, so the 5-second unthrottled drain could not be
  turned off through supported tooling. Fixed in the midware, and `-DrainSeconds` now exists on both
  `measure.ps1` and `run-experiments.ps1`.
