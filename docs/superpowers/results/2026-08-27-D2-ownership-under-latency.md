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

---

## Result: the prediction holds, on both the floor and the ceiling

39 runs, 3 repeats per point. Conservation is exact (400 objects) at **every** point — nothing is
ever lost or duplicated; this axis moves the ownership gap alone.

| `T_L` | L = 8 | L = 16 | L = 32 | predicted min L |
|---|---|---|---|---|
| 0 ms | CLEAN | CLEAN | — | 4 (Phase D) |
| 10 ms | CLEAN | CLEAN | CLEAN | ~6 |
| 25 ms | CLEAN | CLEAN | CLEAN | ~7 |
| **50 ms** | **FAILS** (1772, 1769, 1770) | CLEAN | CLEAN | **~10** |
| 75 ms | — | CLEAN | CLEAN | ~13 |
| **100 ms** | — | **FAILS** (0, 1722, 541) | **CLEAN** | **~16** |

The prediction was written before any of it ran, and both halves land:

- **The floor.** `L = 8` was predicted to hold at 10 and 25 ms and fail around 50. It does, and it
  fails the way the threshold model says it should — not gradually, but by collapsing to ~1770 of
  1800, the same sub-threshold regime `L = 2` produced at zero latency.
- **The ceiling.** `L = 16` was predicted to run out at `T_L ≈ 100 ms` (from `L_min ≈ 4 + T_L/dt`,
  `dt = 8.33 ms`). It is clean at 75 ms and fails at 100 ms, and `L = 32` closes the gap there.

At the crossing point the failure is **bimodal**, which is what a threshold predicts of a marginal
case: `[0, 1722, 541]` across three repeats — one clean, one full collapse, one partial.

**A note on what `ownership_gap_ticks` counts.** At `L = 8 / 50 ms` it took 174 late arrivals to
produce a gap of ~1770; at `L = 16 / 100 ms` just **3** late arrivals produce 1722. The counter
measures the *duration* an object spends unowned, not the number of late events. Phase D's
one-for-one finding was `hoLate > 0` iff `gap > 0` — a causal link, never a proportionality, and
this is the clearest case of the difference.

## The consequence: two guarantees whose bounds cross

At `T_L = 100 ms` the ownership gap needs `L = 32`. Ruling D5's halo bound forbids it:
`v_max * L * dt <= halo_width` gives `L <= 16` at `--halo-width 8`. **The two guarantees cannot
both be satisfied at that band width.** This is no longer an extrapolation — the run that closes
the gap is the run that violates the other bound.

The resolution is a wider band, and the required width is a **compound** of the two bounds that
neither states alone:

| bound | at `T_L = 100 ms` | requires |
|---|---|---|
| Phase C halo soundness — `v_max * (L_halo * dt + T_L) + 2 * r_max` | `60 * (4/120 + 0.1) + 4` | width >= **12** |
| Phase D handoff lookahead — `v_max * L_handoff * dt` | `60 * 32/120` | width >= **16** |

Phase C's bound alone would prescribe 12, which is **not enough** once the handoff lookahead the
ownership guarantee requires is accounted for. The binding constraint at 100 ms is 16, and it
comes from the interaction of the two mechanisms rather than from either.

That interaction is not free. Phase C §4.8 measured that a wider band is **not** always safer
under latency — at 200 ms injected delay, width 12 caught every contact while width 28, its own
bound's prescription, did not, because publishing that many shadows over a delayed link costs
more than the extra width buys. So the compound requirement rises with latency while the width
that is actually safe stops rising, and somewhere above 100 ms they cross for good.

**This is a third instance of the project's recurring shape.** Phase C: raising `--halo-lookahead`
to absorb link latency fails. Phase D: raising `--handoff-lookahead` to absorb load fails. Here:
raising it to absorb latency *works*, but only until it collides with a different guarantee's
bound — the mitigation is correct and still runs out.

## What this does not show

- **One workload, one band width.** Every point is `uniform` at `--halo-width 8`. The crossing
  point is a property of that pair; a different `v_max` moves both bounds.
- **Zero jitter.** `T_J = 0` throughout, as in all 302 of Phase C's runs. Jitter remains
  implemented, asserted and unswept.
- **Below the pacing budget.** 400 objects, `phys_p95` ~1 ms against 8.33 ms. Phase D showed that
  above the budget no lookahead closes the gap at any latency, so this whole table is the
  in-budget regime.
- **Two servers.** The 4-server contention tail Phase D measured is not exercised here.

---

## Follow-up: does jitter bind on its worst case or its mean?

`--link-jitter-ms J` adds a **worst-case** extra delay drawn deterministically from the seed, so
effective one-way delay lands in `[T_L, T_L + J]`. `HaloBound.h` treats it as worst-case — the
published formula is `v_max * (L * dt + T_L + T_J) + 2 * r_max`, adding `T_J` at its maximum, and
`tools/InteractionTests` asserts that. But that is the **halo** bound. Nothing has ever tested
whether the **handoff** lookahead binds the same way, because jitter has never been swept at all:
zero in all 302 of Phase C's runs and all 39 above.

### The discriminator, and the prediction

The two hypotheses are separable with one point, because the sweep above already measured both
candidate answers at zero jitter:

| configuration | effective delay | measured at `L = 8` |
|---|---|---|
| `T_L = 25`, `T_J = 0` | 25 ms flat | **CLEAN** |
| `T_L = 50`, `T_J = 0` | 50 ms flat | **FAILS** |
| **`T_L = 0`, `T_J = 50`** | **uniform in [0, 50], mean 25** | **?** |

- If the lookahead binds on the **mean** (25 ms), `L = 8` should be **CLEAN**.
- If it binds on the **worst case** (50 ms), `L = 8` should **FAIL**.

**Predicted: it binds on the worst case, and `L = 8` fails.** The reasoning is the finding
recorded above — `ownership_gap_ticks` measures *duration*, not event count, and a single late
arrival is enough to open a gap that persists. Three late arrivals produced 1,722 gap ticks at
`L = 16 / 100 ms`. A delay distribution whose tail crosses the threshold will therefore be
governed by that tail however rarely it is drawn, and averaging is the wrong summary.

If instead `L = 8` comes back clean, the threshold model is incomplete: it would mean late
arrivals are being absorbed somewhere rather than each opening a gap, and the one-for-one
causation from Phase D would need re-deriving under jitter.

**Design.** `T_L = 0`, `T_J` in {25, 50} x `L` in {8, 16}, 3 repeats, otherwise identical to the
sweep above. Jitter is drawn from `--seed`, and release times are forced monotonic per target, so
the runs stay reproducible and the delay never reorders a link.
