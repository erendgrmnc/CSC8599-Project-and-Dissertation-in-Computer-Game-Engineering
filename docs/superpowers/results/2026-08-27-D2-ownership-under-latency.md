# Phase D follow-up — the ownership fix under injected link latency

**Date:** 2026-08-27
**Predecessor:** `docs/superpowers/results/2026-08-26-D-ownership.md`

Phase D closed items 7 and 15 and closed item 2 conditionally, but measured all of it at
`T_L = 0`. That leaves §5.1 of the backlog design unhonoured: its entire argument for ordering
Phase D after Phase C was that injected latency is what makes the ownership gap proportional to
link delay and therefore worth measuring. This is that measurement.

`--link-latency-ms` delays the **server-to-server** path, and handoff transfers and custody
resends both reach it: `SendPacketToServer` routes to `QueueDelayedPeerPacket` whenever latency
or jitter is non-zero, and it is the send path for both the transfer
(`DistributedGameServerManager.cpp:1112`) and the resend (`:1629`).

---

## The prediction, stated before the sweep ran

Phase D established that the handoff lookahead is a **threshold**: it must exceed the
delivery-plus-pacing latency, and below that it is worse than no lookahead at all. Injected
one-way latency adds directly to delivery time, so the required lookahead should rise by
`T_L / dt` ticks, with `dt = 8.33 ms`:

| `T_L` | added ticks (`T_L / dt`) | predicted minimum L |
|---|---|---|
| 0 ms | 0 | 4 (measured, Phase D) |
| 10 ms | 1.2 | ~6 |
| 25 ms | 3.0 | ~7 |
| 50 ms | 6.0 | ~10 |

So `L = 8` — the shipped default — is predicted to hold at 10 ms and 25 ms and to **fail around
50 ms**, and `L = 16` to cover all three.

**The interesting part is the ceiling, not the floor.** Ruling D5's halo bound caps the lookahead
at `L <= halo_width * 120 / v_max` = **16** at `--halo-width 8`. Extrapolating the table, `T_L`
of roughly 65-70 ms would demand a lookahead the halo bound forbids. If that holds, then beyond
that latency **the ownership gap and cross-border collision soundness cannot both be satisfied at
this band width** — the same tension Phase D found at capacity load, reached along an independent
axis.

That is the prediction this sweep is designed to refute or confirm. It is written down first
because Phase C's §4.8 records what happens otherwise: a sweep that never samples the predicted
point can only return all-pass or all-fail.

## Design

400 objects, 2 servers, 1800 paced ticks, `--halo-width 8 --halo-reliable`, 30 s drain, 3
repeats — the configuration where `L = 8` is clean at `T_L = 0`.

`T_L` in {10, 25, 50} ms x `L` in {8, 16, 32}. 27 runs. `T_L = 0` is already measured.

`L = 32` is included **knowing it violates the halo bound at this width**: if the gap closes at
32 where it does not at 16, that is the tension made concrete rather than argued. Every point is
checked against the `3 * L` extrapolation clamp and the `30 + L` staleness horizon that Phase C
§4.8 records as separate envelope limits — at these latencies and lookaheads both are clear.
