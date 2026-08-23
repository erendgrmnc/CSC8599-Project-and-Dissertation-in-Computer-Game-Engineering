# Closing the evaluation backlog — prioritised design (2026-08-23)

Scope: every open item in `docs/EVALUATION.md` §5 (what the evidence cannot show), §6 (conditional
guarantees) and §7 (the build-phase backlog), sequenced into four phases.

This is a decomposition, not one feature. The four phases touch independent subsystems and each ends
at a re-measurement, so each gets its own implementation plan. This document fixes the **order** and
the **gates**; it does not specify line-level changes beyond what the ordering argument needs.

---

## 1. The organising principle

**The cost of this backlog is re-measurement, not code.** Every change that can alter a simulation
result invalidates figures taken before it. The project has already paid this once: the server
directories were frozen while E1–E8 were measured, specifically to keep E5's 120 runs valid against a
fixed binary, and the post-freeze work was split into Batch A (changes that provably could not alter
a simulation result) and Batch B (changes that could, followed by one full re-measurement).

That split is the right instrument, and this plan applies it to the whole remaining backlog:

1. **Order by blast radius, not by difficulty.** Everything that cannot alter a simulation result
   goes first, so it can never force a re-run of anything.
2. **Batch the behaviour changes** so one re-measurement sweep covers several items.
3. **Front-load the investigations whose cost grows over time** — a bisect gets more expensive with
   every commit added to its search window.
4. **Every behaviour batch passes a no-op gate before any claim is made.** With its new flag at the
   default, a paced run must reproduce the pre-change baseline exactly. This is Batch B's Step 2, a
   hard gate that ran before any custody result was claimed. Batch A had no such gate — it was
   selected on the argument that none of its items *could* alter a result, and the one that did
   (item 4, the halo publish rate) was caught only afterwards, by re-measuring E2 and E5's L=24 knee
   against the new binary. The gate is cheaper than the re-measurement that substitutes for it.

The phases are ordered A → B → C → D by that principle. Two of the orderings are load-bearing and
argued below (§3.1, §5.1); the rest follows from blast radius.

---

## 2. Phase A — instrumentation and harness

**Closes:** backlog items 10 and 11; E8's overhead-model caveat; E3's "absolute counts not quotable"
caveat.

**Blast radius: none.** These changes read counters ENet already maintains and add processes to the
harness. Nothing alters what a server simulates or sends.

### 2.1 Count real datagrams (item 10)

E8's verdict at one client flips depending on whether traffic is costed at payload or
payload-plus-headers, and ENet's command coalescing sits between the two. The fix is to stop
modelling and read what ENet actually sent.

The decisive structural fact: **snapshot traffic and halo traffic go over separate ENet hosts.**
Snapshots leave through `mDistributedPacketSenderServer` (a `GameServer` host); halo updates and
handoffs leave through `mDistributedPhysicsClients`, one `GameClient` host per peer
(`DistributedGameServerManager.cpp:865-887`). So per-host totals split along exactly the line E8
needs, with no attribution guesswork.

- Add public accessors to `NetworkBase` for the host's `totalSentData` / `totalSentPackets` (and the
  received counterparts). `netHandle` is `protected` there and `NetworkBase.cpp` already includes
  `enet.h`, so the declaration goes in the header and the definition in the `.cpp` — no ENet types
  leak into headers, preserving the existing forward-declaration discipline.
- Add to the server `@@FINAL` line: client-facing bytes/packets from the sender-server host, and
  peer-facing bytes/packets summed across the peer links.
- Teach `analyse.py` to read them, report B/s, and **cross-check against the existing modelled
  figure** rather than replacing it silently. A disagreement between measured and modelled bytes is
  itself a result — it is the size of the coalescing effect.

**Known limit to state in the results:** `totalSentData` is `enet_uint32`, so it wraps after 4 GB —
about 15 minutes at E8's measured 4.6 MB/s. E8's runs are 20 s, so this is safe, but any longer run
needs the counter accumulated into 64 bits at sample time rather than read once at exit.

**Attribution caveat to state:** the client-facing host also carries acks, spawns and manifest
traffic, and the peer host also carries handoffs. On E8's configuration (`uniform`, halo on, no
interaction drivers) snapshots and halo dominate their respective hosts, and the non-dominant
components are bounded by counters already reported (`hoSent`, `manifestSent`). Report the totals as
per-host, not as "snapshot bytes" and "halo bytes".

### 2.2 More than one client (item 11)

E8's client-count argument — that the saving scales with clients while the halo cost does not — is
currently an analytical extrapolation from how the counters increment, because the harness starts one
client. `measure.ps1:148` hardcodes `--clients 1` to the manager and starts a single client at
`:182`.

- Add `-Clients N`, passing `--clients N` to the manager and starting N client processes.
- The `-LateClientAfter` path at `measure.ps1:186-189` already starts a second client successfully,
  so the multi-client bootstrap is known to work; this generalises it rather than proving it.
- Per-client logs must be distinct (`cli-0.log` … `cli-N.log`).
- **`analyse.py:275` reads a fixed list `("mid.log", "cli.log")`.** It must read the per-client logs.
  Note that a naive `cli*.log` glob would newly include `cli-late.log`, which is currently *not*
  read — that would silently change the I4 command-accounting tally. Match the numbered clients
  explicitly.

### 2.3 Re-runs

| Experiment | Configuration | Purpose |
|---|---|---|
| E8 | `exp-bytes-clean` as published, at 1 **and** 2 clients | Settle the payload-vs-datagram ambiguity and convert the client-scaling argument from analytical to measured |
| E3 | as published, with `--drain-seconds 0` | Free: item 5 is already fixed, and this makes E3's absolute counts quotable |

Both are 20 s realtime runs at 3 repeats — the cheapest runs in the suite.

### 2.4 Gate

A paced `--run-ticks --fixed-step` run must reproduce the current baseline's end state exactly. These
changes should not be able to affect it; the gate is what makes that a verified statement rather than
an assumption.

---

## 3. Phase B — attribute the E4 regression (item 12)

**Closes:** item 12's attribution. Produces an answer, not code.

`runs/exp-fix4-E4` loses 84–703 objects on a workload where the pre-batch baseline (`exp-balance`,
commit `93e6f21`) was exact. The custody work is a suspect but was explicitly not blamed, because
three separate behaviour changes landed across the same window.

### 3.1 Why this is second, not last

Two reasons, and the first is the ordering argument:

1. **A bisect's cost grows with every commit added to its window.** Phases C and D both land
   behaviour changes on the same conservation-sensitive path. Running the bisect afterwards means
   searching a strictly larger window, with more candidate causes and more interactions between them.
2. **It decides where the fix belongs.** If the cause is custody, the fix lands in Phase D alongside
   the ownership work. If it is the halo publish gate or drain-seconds, it is an earlier fix and
   Phase D inherits a clean baseline.

### 3.2 It is a three-point comparison, not a binary search

`93e6f21..58d3b08` is 46 commits, but 33 touch only `docs/`. Exactly three clusters can alter a
simulation result:

| Candidate | Commit(s) | Mechanism to suspect |
|---|---|---|
| Halo publish gate | `fd98f97` | Changed how much halo traffic is emitted per tick |
| Drain-seconds forwarding | `2e7c65e` (+ `a8a9570` harness) | Changed how a run ends, and in-flight transfers are lost at exit |
| Custody | `5f0b809` … `58d3b08` | Reclaim and idempotent-arrival paths touch ownership directly |

So the procedure is: build each of the three points, run E4's configuration (`cluster`, 4,000
objects, 2 servers, 7,200 paced ticks, 3 repeats) at each, and compare `conservation_delta`. That is
9–12 runs plus three builds, not a binary search over 46 commits.

**Expected confounder:** `--drain-seconds` did not exist before `2e7c65e`, so the pre- and
post-commit runs do not end the same way. Hold the drain explicitly constant where the flag exists,
and state the asymmetry where it does not, rather than comparing two different run terminations and
attributing the difference to the code.

### 3.3 Gate

None — this phase changes no production code. Its output is an attribution recorded in
`docs/superpowers/results/`, plus a fix routed to whichever phase owns the cause.

---

## 4. Phase C — latency injection and the generalised soundness bound

**Closes:** §5 "no latency injection"; the AP §6 finding that E5 validates a bound whose latency term
is absent.

**Blast radius: default-off.** A new flag defaulting to zero delay, plus a formula that must return
its current value at zero latency.

### 4.1 Why this matters more than its size suggests

The AP benchmark established that this project's soundness condition

```
w_min = HALO_ASSUMED_MAX_SPEED * L * dt + 2 * HALO_ASSUMED_MAX_RADIUS
```

(`ServerWorldManager.cpp:672-680`) is a **zero-latency, zero-frame-jitter special case** of the
ancestor's condition

```
T_T = (3 * ceil((2*T_F + T_L) / T_P) - 1) * T_P
R_a = R_o + V_t * T_T
```

That converts "no latency injection" from a missing datapoint into a gap in the headline claim: E5
validates a bound whose latency term is absent because latency was always zero. A reviewer will find
this; the AP results document says so explicitly.

Confirmed by grep: **no latency injection exists anywhere** in the servers, client or harness. This
is new code, not a flag to expose.

### 4.2 Work

- `--link-latency-ms M` and `--link-jitter-ms J` on the game server. Both must be parsed in
  `ServerStarter.cpp` **and** forwarded in `PhysicsServerMidware/ProgramStart.cpp` — game servers are
  spawned by the midware, so a flag the midware does not forward is silently ignored with no error.
  The forwarding pattern is the existing `if (config.Has(...)) serverExtraArgs += ...` block at
  `ProgramStart.cpp:60-100`.
- Generalise `MinimumSafeHaloWidth` to carry the latency and frame-jitter terms, such that today's
  expression falls out at `T_L = 0`. It is one function in one place, so the code change is small;
  the cost is entirely in re-validation.
- Make `HALO_STALE_TICKS` track `--halo-lookahead`, or warn when it does not (see §4.4 warning 2).
  This is a prerequisite for the sweep, not an optional tidy-up.
- Delay applies to the server-to-server path at minimum — that is what the halo bound is about.
  Whether to delay the client path too is a plan-level decision, not a design one; if it is included
  it must be separately switchable, because it changes E3 and E8 and not E5.

### 4.3 Gate

Two conditions, both hard:

1. At `--link-latency-ms 0`, a paced run reproduces the pre-change baseline exactly.
2. The generalised `w_min` returns the **same value** as the current formula at zero latency, checked
   in `tools/InteractionTests` — the bound is pure arithmetic and belongs in the assert harness.

### 4.4 Re-runs, and how to keep them affordable

E5's published sweep is 120 runs at zero latency. Crossing a full latency dimension into it would
triple that. It should not be crossed fully:

- The existing zero-latency sweep stays valid once gate 4.3.1 passes, and is the baseline.
- Add latency only at the lookaheads whose knee is genuinely **bracketed** — L=8, L=16 and L=24,
  whose round-2 knees (3, 4, 6) each sit between a measured failing width and a measured passing
  width. L=2 is not one of them: its knee is only bounded above (≤1) because the width axis bottoms
  out at 0, so a latency dimension there cannot show a transition either.
- **Exclude L=32.** It is outside the implementation's envelope, not a soundness result:
  `HALO_STALE_TICKS = 30` (`ServerWorldManager.cpp:944`) retires a shadow at `senderTick + 30`,
  before a lookahead of 32 would ever apply it, so the halo is effectively disabled at any lookahead
  above 30 regardless of width.
- At each added latency, sweep width **upward from 0**, which is round 2's own methodology: round 1
  swept *around* the predicted floor and could therefore only return all-pass or all-fail, never
  locating the transition. That sampling artefact was published as "the knee moved right" before it
  was caught.

That is roughly 72 runs rather than 360, and it tests what the bound actually claims: that the knee
tracks the *generalised* floor as latency rises, not merely that a knee exists.

**Two methodological warnings for this sweep, both from round 2's own findings:**

1. **The slack widens with lookahead** (≥4, 5, 8, 10 at L=2/8/16/24) — the conservative bound gets
   *more* conservative as lookahead grows, and every measured knee sits well below its floor. So
   raising the predicted floor by adding a latency term does not by itself predict that the measured
   knee moves. The sweep must be ranged to detect a knee that moves *less* than the floor does; a
   range positioned by the new floor alone will miss it, which is exactly round 1's error repeated
   with a different offset.
2. **`HALO_STALE_TICKS` is a hard ceiling on this phase.** If a latency term raises the lookahead
   needed for soundness above 30 ticks, the configuration silently leaves the envelope and the halo
   stops working — the failure looks like unsoundness but is a staleness horizon. The E5 results
   document already recommends the fix as build-phase work: raise `HALO_STALE_TICKS` to track
   `--halo-lookahead`, or warn when it does not. **Phase C must do one of those before it sweeps**,
   or it risks reporting an envelope limit as a falsification of its own bound.

### 4.5 Optional companion

§5 records that `headon` is one synthetic workload — fixed lanes, one speed, perpendicular approach,
the configuration where the halo's knee is sharpest. An oblique or mixed-speed variant would stress
the bound harder. This is the natural companion to §4.4 and should be scoped as optional: it widens
the claim but is not required to close the latency gap.

---

## 5. Phase D — ownership

**Closes:** backlog items 2 and 7; §6's conditional ownership guarantee.

**Blast radius: maximum.** Changing the default handoff protocol invalidates conservation-sensitive
figures from nearly every experiment, since most ran at `--handoff-lookahead 0`.

### 5.1 Why after C, not before

At zero link latency the ownership gap is one near-instantaneous round trip. Closing it would look
like a no-op: the measurement would show little, because there is little to show. Phase C's injected
latency makes the gap proportional to `T_L`, which is what gives Phase D a measurement worth
reporting — the fix has to be visibly fixing something.

This is also the honest scientific order. The gap is currently defended as conditional but bounded;
latency injection is the experiment that tests whether that defence survives realistic conditions.

### 5.2 Work, bundled deliberately

Both items land together so one re-measurement covers them:

- **Item 7** — `CalculateIncomingObjectOffsetPosition` (`ServerWorldManager.cpp:440`) currently
  computes the clamp and discards it, incrementing `hoClamp` when it would have moved an object. The
  body is correct; wiring the result in changes measured handoff behaviour, which is why it was
  deferred to land with the ownership work rather than as a drive-by.
- **Item 2** — make scheduled release (`--handoff-lookahead > 0`) the default, so release-on-send
  stops being the normal case and the window in which nobody owns an object stops being
  unconditional.

### 5.3 Gate

The no-op gate applies in an unusual form here, because the point of the change *is* to alter the
default. Gate on the **old** default instead: at an explicitly passed `--handoff-lookahead 0`, a
paced run must still reproduce the pre-change baseline exactly. That separates "the new default
behaves differently" (intended) from "the old path changed" (a defect).

Additionally, `analyse.py`'s `check_custody` reports a non-zero `hoCustody` at exit as an invariant
failure, and that check must not be weakened to make these runs pass. A run intended to pass the gate
is drained until `hoCustody` reads 0.

### 5.4 Re-runs

Full sweep, as approved:

| Experiment | Why it must be re-run |
|---|---|
| E4 | Conservation-sensitive; the rebalancing regression lives here |
| E7 | Conservation-sensitive; ownership gaps become near-permanent past the budget |
| E1 | Regression — locality should be untouched, and that should be shown, not assumed |
| E2 | Regression — border crossings are the halo's acceptance test, which handoff timing touches |

E3, E6 and E8 are not conservation-sensitive and do not need re-running for this phase, provided the
Phase D gate (§5.3) passes.

---

## 6. What is explicitly out of scope

These are open items in §5 that are **not** code, and the plan should say so rather than leaving them
looking unaddressed:

- **The single-machine confound.** Every run puts every role on one 6-core box, so the 2→4 server
  segment carries process contention as well as protocol cost. Separating them needs one server per
  machine — a run configuration, not a change. Nothing in the code has to change for it.
- **Tick-epoch divergence and invariant I8.** Halo scheduling is expressed in the sender's tick
  numbers, and two servers share no epoch once either stops holding its pacing budget. This cannot be
  cured at that layer; it needs both servers inside their pacing budget, which is a load and hardware
  question. It stays a documented limit.

Both remain in §5 as stated limits. Phase C's latency work does not address them and must not be
described as though it does.

---

## 7. Summary

| Phase | Items closed | Can alter a result? | Re-runs | Gate |
|---|---|---|---|---|
| A — instrumentation & harness | 10, 11, E3 caveat, E8 caveat | No | E8 (×2 client counts), E3 | Paced run reproduces baseline exactly |
| B — attribute E4 regression | 12 | No (no production code) | 3 candidate points × E4 | None |
| C — latency & generalised bound | §5 latency, AP §6 finding | Default-off | E5, partial sweep | Zero-latency no-op + arithmetic identity |
| D — ownership | 2, 7, §6 conditional guarantee | Yes, by design | E1, E2, E4, E7 | Old default reproduces baseline exactly |

The ordering is load-bearing in two places: **B before C and D**, because a bisect window only grows;
and **C before D**, because latency is what makes the ownership fix measurable. A and B can be worked
concurrently — A touches instrumentation and the harness, B touches neither.
