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
   default, a paced run must reproduce the pre-change baseline **on the fields that are actually
   stable** (see §1.3 — exact reproduction is not available). This is Batch B's Step 2, a
   hard gate that ran before any custody result was claimed. Batch A had no such gate — it was
   selected on the argument that none of its items *could* alter a result, and the one that did
   (item 4, the halo publish rate) was caught only afterwards, by re-measuring E2 and E5's L=24 knee
   against the new binary. The gate is cheaper than the re-measurement that substitutes for it.

The phases are ordered A → B → C → D by that principle. Two of the orderings are load-bearing and
argued below (§3.1, §5.1); the rest follows from blast radius.

### 1.1 There is no stored baseline — every phase must make its own

**`runs/` does not exist in this working copy.** It is gitignored (`.gitignore:142`), so no dataset
the evaluation cites was ever committed: `exp-balance`, `exp-fix4-E4`, `exp-fix4-E7`,
`exp-bytes-clean`, `exp-capacity-halo`, `exp-capacity-nohalo`, `exp-ap-injection-paced` are all
absent. The **figures** survive, transcribed into `docs/EVALUATION.md` and the results documents; the
**per-tick CSVs and `@@FINAL` lines behind them do not**.

Two consequences, and both bind every phase:

1. **Every no-op gate in this plan compares a new run against a baseline that is not on disk.** So
   each phase begins with **step 0: build current HEAD and run its own baseline**, before any change
   lands. A gate is only as good as a baseline generated from the commit it is gating against, which
   makes this a correctness requirement rather than bookkeeping.
2. **Nothing can be settled by re-analysis.** Any question of the form "do the old numbers still say
   X if we recompute Y" — E3's drain-artefact ratios, E4's loss against `ho_parity_delta`, E8's
   modelled bytes — requires a re-run, not a re-read. Where this plan says "check", it means "run".

This does not change the phase order. It adds a fixed cost to the front of each phase and removes
re-analysis from the menu of cheap options.

### 1.2 A phase is not done until the documents it invalidates are corrected

Each phase below carries a **Documentation to correct** subsection listing the specific claims that
phase falsifies, with the file and the claim named. Those are tasks, not reminders: a phase that
lands its code and leaves the prose asserting the old behaviour has moved the defect rather than
fixed it.

This is not hypothetical bookkeeping in this repository. `CLAUDE.md` carries a standing
"Verified-state warnings" section that exists **only** because documentation repeatedly drifted from
the code, and the assessment behind this revision found three more instances: `CLAUDE.md` asserting
the incoming-offset body was correct when it is not (§5.3), `docs/EVALUATION.md` describing figures
as traceable to run directories that were never committed (§1.1), and
`docs/SPATIAL-PARTITIONING.md` describing a function by a name it no longer has, at line numbers it
no longer occupies, in a state it is no longer in (§2.5, §5.5). The first two are corrected as of
2026-08-23; the rest are scheduled into the phase that touches the relevant code.

The rule: **the doc correction lands in the same change as the code, not in a follow-up pass.** A
follow-up pass is what produced the backlog of stale claims in the first place.

### 1.3 Paced runs do not reproduce exactly — measured, 2026-08-23

Every gate in this document was originally written as "a paced run must reproduce the
pre-change baseline exactly". **That instrument does not exist.** Measured during Phase A
execution on four clean runs at the identical commit, seed and configuration (`uniform`,
400 objects, 2 servers, 1,800 paced ticks, `--halo-width 8 --halo-reliable`):

| behaviour | fields |
|---|---|
| identical on every clean run | the 21 zero-valued or tick-locked counters — `cmdApplied` `cmdRelayed` `cmdDup` `cmdRejected` `cmdFanout` `hoFail` `hoLate` `hoResent` `hoReclaimed` `hoCustody` `hoDup` `hoPending` `hoSched` `haloLate` `haloAhead` `haloSent` `haloRecv` `manifestSent` `objPreseed` `objSpawned` `objDestroyed` |
| per-server varies, world total stable | `objs`, `objPool` — the 400 objects split 201/199 or 200/200 |
| varies outright | `contacts` (~1%), `snapSent` (~1.5%), `haloObjSent`/`haloObjRecv` (~10%), `hoSent`/`hoRecv` (±1), `objFwd`, `objHalo`, `objWorld`, `hoClamp` |

`ownership_gap_ticks` across those four runs was 84, 82, 91, 84.

**This contradicts `CLAUDE.md`**, which states that under `--run-ticks --fixed-step` "end
state and conservation then reproduce exactly", and that halo runs are reproducible under
`uniform`. Conservation reproduces — the world total is 400 every time. End state does not.

A fifth run degraded badly (opening frame times of 1,747 ms and 1,049 ms against an 8.33 ms
budget), cascading into custody firing, `haloLate` 9,530, `ownership_gap_ticks` 1,779 and two
ticks of **double ownership**. `analyse.py` caught it with a REPRODUCIBILITY WARNING, so such
a run is detectable — but its end state resembled nothing else.

**What every phase's gate must therefore be:**

1. **A validity precondition.** Both sides certified clean by `analyse.py` — no reproducibility
   warning, no custody firing. A degraded run is a failed measurement, not a comparand, and
   must be discarded and re-run rather than compared.
2. **Exact equality on the stable set** above, plus world-total conservation.
3. **Range comparison on the varying fields**, across at least three repeats per side.
4. **A structural argument** for whatever the numbers cannot reach.

`tools/gate-compare.py` (added in Phase A) implements 1–2.

**This also bears on the published evidence.** E1–E8 figures came from runs whose logs are
gone (§1.1), so it cannot now be checked whether any of them was a degraded run of the kind
above. Nothing here shows that one was; what it shows is that the check was available and
there is no record of it having been applied.

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

**This does not improve the overhead model — it removes the need for one.** `totalSentData` is
incremented with the return value of `enet_socket_send` (`enet/protocol.c:1732-1733`), which is the
byte count actually written to the socket for **one datagram, after ENet has coalesced every queued
command for that peer into `host->buffers`**, including the ENet protocol header. `totalSentPackets`
increments once per datagram alongside it. So coalescing — the whole reason the payload and
payload-plus-header models disagree — is already measured, and real wire cost is:

```
wire bytes = totalSentData + 28 * totalSentPackets      (IPv4 20 + UDP 8)
```

The published model added a flat 36 B per *packet*, which is correct only if every packet became its
own datagram. The gap between that figure and this one is the size of the coalescing effect, and is
worth reporting as a result in its own right rather than silently replacing the old number.

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
- Teach `analyse.py` to read them, report B/s, and **report the modelled figure alongside** rather
  than replacing it silently, for the reason given above.

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

### 2.3 Tick-epoch alignment — an existing mechanism nothing has used

Not a backlog item; found while assessing them, and it belongs here because it is a **flag, not a
code change**, so it costs runs and nothing else.

`HeadlessRunner.cpp:88-97` implements `--epoch-align-us`: at tick 0 each server spins to the next
shared boundary on the monotonic clock, so every server's tick 0 lands on the same instant. Its own
header comment gives the motivation — the game-start broadcast "arrives with a spread of a few
milliseconds, which at 120 Hz is enough to shift epochs by a tick and make handoff scheduling differ
between runs".

It landed 2026-08-17, **before** the E1–E8 measurement pass. It defaults to `0` (disabled) in
`measure.ps1:49` and `run-experiments.ps1:67`, and no experiment document sets it. So the published
runs were almost certainly taken without it. That cannot be confirmed from the run manifests — they
record `epochAlignUs`, but they are in the missing `runs/` (§1.1).

This matters because both handoff scheduling (`senderTick + lookahead`) and halo scheduling are
expressed in the *sender's* tick numbers, and are meaningful to a receiver only if both servers agree
on the epoch. It is a candidate explanation for part of the ±1 handoff-event variance that the
reproducibility notes attribute to needing a global tick barrier.

**Work:** none in the servers. Run the Phase A baseline both with and without alignment and compare
handoff-event variance across repeats. If it tightens, enable it for every subsequent reproducible
run in this plan and record that in the experiment suite. Note it does **not** address the *drift*
half of tick-epoch divergence (§6) — only the start-of-run offset.

### 2.4 Re-runs

Step 0 (§1.1): build HEAD and take the baseline these gates compare against.

| Experiment | Configuration | Purpose |
|---|---|---|
| E8 | `exp-bytes-clean` as published, at 1 **and** 2 clients | Settle the payload-vs-datagram ambiguity and convert the client-scaling argument from analytical to measured |
| E3 | as published, with `--drain-seconds 0` | Item 5 is already fixed, and this makes E3's absolute counts quotable |
| Epoch alignment | Phase A baseline, `--epoch-align-us` on and off | §2.3 — does alignment tighten run-to-run variance |

The first two are 20 s realtime runs at 3 repeats — the cheapest in the suite.

### 2.5 Documentation to correct

Two of these are stale *today*, independent of anything Phase A changes, and are included here
because Phase A is the phase with no blast radius — the cheapest place to land a prose fix.

| File | Claim to correct |
|---|---|
| `docs/SPATIAL-PARTITIONING.md:7` and its summary table | "The simulated world is a fixed square … **−150 to +150 on each axis**". World bounds are set by `--world` and have been since that flag landed. |
| `docs/SPATIAL-PARTITIONING.md:50` | The handoff acknowledgement described as "**scaffolded**", "partly stubbed", with the ack a `TODO` and the receive handler "commented out". The ack path is live: the receiver acks on acceptance and the sender holds the transfer in custody until it arrives. |
| `CLAUDE.md` game-server flag table | `--epoch-align-us` is missing from it entirely (§2.3), despite being parsed in `ServerStarter.cpp:188` and forwarded by the midware. |
| `docs/superpowers/specs/2026-08-19-experiment-suite.md` | Record whether epoch alignment is on for reproducible runs, once §2.3 has measured it. |

Then, **after** the re-runs land:

| File | Claim to correct |
|---|---|
| `docs/EVALUATION.md` §3, E8 | The overhead-model caveat ("whether the composition claim holds at a *single* client depends on whether datagrams are costed at payload or payload-plus-headers") and the §7 item 10 that records it. Both close on measured datagram counts. |
| `docs/EVALUATION.md` §3, E3 | The "contains an unverified drain-phase artefact; absolute counts not yet clean to quote" footnote, and the caveat paragraph under it. |
| `docs/EVALUATION.md` §7 item 11 | Closes once the harness runs more than one client. |

### 2.6 Gate

A paced `--run-ticks --fixed-step` run must reproduce the step-0 baseline's end state exactly. These
changes should not be able to affect it; the gate is what makes that a verified statement rather than
an assumption. Epoch alignment (§2.3) is held at whatever the baseline used — it is being *measured*
here, so it must not vary inside the gate.

---

## 3. Phase B — attribute the E4 regression (item 12)

**Closes:** item 12's attribution. Produces an answer, not code.

`runs/exp-fix4-E4` loses 84–703 objects on a workload where the pre-batch baseline (`exp-balance`,
commit `93e6f21`) was exact. The custody work is a suspect but was explicitly not blamed, because
three separate behaviour changes landed across the same window.

### 3.0 First establish that there is a regression at all

**Do not start with the bisect.** The premise — that this loss is caused by a code change — has never
been tested, and there is a cheaper explanation that fits the evidence:

- `measure.ps1:53` defaults `-DrainSeconds` to `-1`, which omits the flag, so the server falls back
  to its own default of **5 seconds** (`ServerStarter.cpp:222`).
- Five seconds was already found insufficient for E7, whose residual loss turned out to be
  **end-of-run truncation of transfers still in flight** — `ho_parity_delta` matched it exactly on
  every repeat — and was explicitly reported as explained, not as a defect.
- E4 is the rebalancing workload. Its migration bursts are larger than E7's steady-state handoffs, so
  it has *more* reason to still be draining at exit, not less. It also ran at
  `--handoff-lookahead 300`, which defers every release and so maximises the number of transfers
  still in flight when the run stops.
- **`EVALUATION.md` §7 item 12 already records the truncation signature**: "`ho_parity_delta` matches
  the loss exactly on each repeat and transfers are still held in custody at exit". That is the same
  evidence which, on E7, was accepted as showing end-of-run truncation rather than an open failure
  mode. The two are read differently in the same document.

What has *not* been tested is whether a drain long enough to empty custody removes the loss. That is
the cheapest discriminator available, and it is a run, not an argument.

**Step 1 is therefore one configuration, not a bisect:** re-run E4 at HEAD with a drain long enough
that `hoCustody` reads 0 at exit. If conservation goes exact, the loss was in-flight transfers
truncated by a 5-second drain, item 12 closes as a harness artefact, and §3.1–§3.2 never run. If it
survives a clean drain, the regression is real and the bisect below is justified.

This ordering costs one run to potentially delete an entire phase.

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

The right bisect points are therefore not those commits themselves but the buildable states
*between* them, which is what `EVALUATION.md` §7 item 12 already proposes — `776115b`, `02e306b` and
HEAD. In commit order those separate the candidates cleanly:

| Point | Position | Isolates |
|---|---|---|
| `93e6f21` | pre-Batch-A, the published `exp-balance` baseline | the reference "exact" run |
| `776115b` | immediately before `fd98f97` | behaviourally equivalent to the baseline |
| `02e306b` | after the halo gate and the drain fix, before custody | halo gate + drain, without custody |
| HEAD | after the custody cluster | everything |

A departure appearing between `776115b` and `02e306b` implicates the halo gate or the drain fix; one
appearing between `02e306b` and HEAD implicates custody. A fourth point between `fd98f97` and
`2e7c65e` separates halo from drain, and is only worth building if the first split lands there.

**`93e6f21` must be run, not cited.** The published comparison is against `exp-balance`, whose data
no longer exists (§1.1), so the "exact baseline" this bisect measures a departure from has to be
regenerated. Running E4's configuration (`cluster`, 4,000 objects, 2 servers, 7,200 paced ticks, 3
repeats) at four points is 12 runs and four builds — not a binary search over 46 commits.

Hold the drain constant across all four points at whatever §3.0 established, or this bisect
re-measures the same truncation it was meant to rule out.

**Expected confounder:** `--drain-seconds` did not exist before `2e7c65e`, so the pre- and
post-commit runs do not end the same way. Hold the drain explicitly constant where the flag exists,
and state the asymmetry where it does not, rather than comparing two different run terminations and
attributing the difference to the code.

### 3.3 Documentation to correct

| File | Claim to correct |
|---|---|
| `docs/EVALUATION.md` §7 item 12 | Currently ends "That bisect is the next step, not yet done." Replace with the outcome: closed as a drain artefact (§3.0), or the attribution the bisect produced. |
| `docs/EVALUATION.md` §6 | The paragraph attributing E4's loss to an unisolated cause across the halo-gate / drain-seconds / custody window. |
| `docs/EVALUATION.md` §7 item 12 | If §3.0 closes it, also reconcile the reading of `ho_parity_delta`: the same signature is currently treated as *explained* for E7 and *unexplained* for E4 in the same document. |
| New `docs/superpowers/results/` entry | The pre-check and, if it ran, the bisect — recorded like the other batches. |

### 3.4 Gate

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

### 4.2 Precondition: lift the staleness ceiling first

`HALO_STALE_TICKS = 30` (`ServerWorldManager.cpp:944`) retires a halo shadow that has not been
refreshed for 30 ticks. At the 120 Hz substep that is **250 ms**, and it is a hard ceiling on
lookahead: a shadow deferred to `senderTick + L` for any `L > 30` is retired before it is ever
applied, which is exactly how L=32 came to read as unsound in E5 round 1.

The arithmetic makes this binding immediately, not eventually. Latency costs roughly one tick of
lookahead per 8.33 ms, so:

| injected one-way latency | extra lookahead ticks | L=24 becomes | vs ceiling of 30 |
|---|---|---|---|
| 0 ms | 0 | 24 | fits |
| 25 ms | ~3 | 27 | fits |
| 50 ms | ~6 | 30 | **at the ceiling** |
| 100 ms | ~12 | 36 | **outside the envelope** |

So a sweep that injects more than about 50 ms would leave the implementation's envelope and report
a staleness horizon as a falsification of its own bound — the same misreading E5 round 1 made, at a
different point on the axis. The E5 results document already names the fix as build-phase work:
raise `HALO_STALE_TICKS` to track `--halo-lookahead`, or warn when it does not.

**This is the first task of Phase C, not an item in its work list.** Until it is done, the latency
axis cannot be swept past roughly 50 ms, which is most of the interesting range.

### 4.3 Work

- `--link-latency-ms M` and `--link-jitter-ms J` on the game server. Both must be parsed in
  `ServerStarter.cpp` **and** forwarded in `PhysicsServerMidware/ProgramStart.cpp` — game servers are
  spawned by the midware, so a flag the midware does not forward is silently ignored with no error.
  The forwarding pattern is the existing `if (config.Has(...)) serverExtraArgs += ...` block at
  `ProgramStart.cpp:60-100`.
- Generalise `MinimumSafeHaloWidth` to carry the latency and frame-jitter terms, such that today's
  expression falls out at `T_L = 0`. It is one function in one place, so the code change is small;
  the cost is entirely in re-validation.
- Delay applies to the server-to-server path at minimum — that is what the halo bound is about.
  Whether to delay the client path too is a plan-level decision, not a design one; if it is included
  it must be separately switchable, because it changes E3 and E8 and not E5.

### 4.4 Gate

Two conditions, both hard:

1. At `--link-latency-ms 0`, a paced run reproduces the pre-change baseline on the stable field set
   defined in §1.3. Exact reproduction is not achievable — see that section.
2. The generalised `w_min` returns the **same value** as the current formula at zero latency, checked
   in `tools/InteractionTests` — the bound is pure arithmetic and belongs in the assert harness.

### 4.5 Re-runs, and how to keep them affordable

E5's published sweep is 120 runs at zero latency. Crossing a full latency dimension into it would
triple that. It should not be crossed fully:

- The existing zero-latency sweep stays valid once gate 4.4.1 passes, and is the baseline.
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

### 4.6 Documentation to correct

| File | Claim to correct |
|---|---|
| `docs/EVALUATION.md` §5 | The "**No latency injection.** Every measurement runs on a local network with effectively zero link latency" bullet. |
| `docs/EVALUATION.md` §4 | The soundness condition is stated there as `w_min = v_max * L * dt + 2*r_max`. It becomes the zero-latency special case of the generalised form, and §4 is where the headline correctness result is argued. |
| `docs/superpowers/results/2026-08-19-E5-soundness.md` | Add the latency dimension's results; do not rewrite the zero-latency rounds, which remain valid. |
| `docs/superpowers/results/2026-08-21-AP-injection.md` §6 | It predicts exactly this generalisation ("This should appear in the paper as a structural relationship rather than be found by a reviewer"). Record that it was done, and where. |
| `CLAUDE.md` halo-flags block | Add `--link-latency-ms` / `--link-jitter-ms`, and state the `HALO_STALE_TICKS` ceiling on lookahead (§4.2) — the flag table currently documents the width floor but not the lookahead ceiling. |
| `docs/superpowers/results/2026-08-19-E5-soundness.md` | Its "Server code is frozen for this evidence" note on the `HALO_STALE_TICKS` fix closes when §4.2 lands. |

### 4.7 Optional companion

§5 records that `headon` is one synthetic workload — fixed lanes, one speed, perpendicular approach,
the configuration where the halo's knee is sharpest. An oblique or mixed-speed variant would stress
the bound harder. This is the natural companion to §4.5 and should be scoped as optional: it widens
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

- **Item 7** — `CalculateIncomingObjectOffsetPosition` currently computes the clamp at
  `ServerWorldManager.cpp:2019` and discards it, incrementing `hoClamp` when it would have moved an
  object. Wiring the result in changes measured handoff behaviour, which is why it was deferred to
  land with the ownership work rather than as a drive-by. **But the body is not correct, and must be
  fixed before it is wired in** — see §5.3.
- **Item 2** — make scheduled release (`--handoff-lookahead > 0`) the default, so release-on-send
  stops being the normal case and the window in which nobody owns an object stops being
  unconditional.

### 5.3 The clamp's Z bound contradicts the ownership rule

Found while assessing item 7, and it changes the item from "wire it in" to "fix it, then wire it in".

`CalculateIncomingObjectOffsetPosition` clamps Z with an **inclusive** upper bound:

```cpp
// Z's upper bound is inclusive, so max is legal and needs no epsilon.
const float highZ = std::max(mServerBorderData->minZVal, mServerBorderData->maxZVal);
```

and its header comment (`ServerWorldManager.cpp:2325-2326`) states that the bounds "mirror
`IsObjectInBorder` exactly - half-open on X (>= min, < max), closed on Z (>= min, <= max)".

**That mirroring claim is stale.** `IsObjectInBorder` now delegates through `GetObjectServer` to
`NCL::Interaction::OwningServerFor`, which is half-open on **both** axes — `point.z < region.maxZ`,
with the world's outer maximum closed as the only exception. So on an interior Z seam the clamp
produces a position that `IsObjectInBorder` rejects: it would place an incoming object on a
coordinate a *different* server owns. That is precisely the disowned-object failure the ownership
unification was written to eliminate, reintroduced on the one path that had not yet been unified.

The same staleness appears in `CLAUDE.md`'s audit note, which records that "the body itself is now
correct (it clamps into the region using bounds that mirror `IsObjectInBorder`)". Both should be
corrected together.

**Reachability — it is masked at 2 servers and live at 4.** `GameInstance::CalculateServerBorders`
(`DistributedPhysicsServerDto.cpp:94-137`) builds a 2-D grid: `numCols = ceil(sqrt(serverCount))`,
`numRows = ceil(serverCount / numCols)`.

| servers | grid | interior Z seam? | clamp's Z bound |
|---|---|---|---|
| 2 | 2 × 1 | no — `maxZ == worldMaxZ` | harmless (outer edge is closed) |
| 4 | 2 × 2 | **yes** | **produces a position owned by another server** |

Every conservation measurement to date ran at 2 servers, which is why nothing has caught it, and why
it stays invisible for as long as the result is discarded.

**Work:** make the Z bound half-open with the same `INWARD_EPSILON` treatment X already gets, so the
clamp lands strictly inside the region under `OwningServerFor`'s rule. The world-outer-edge exception
must be preserved. This belongs in `tools/InteractionTests` — the clamp is pure geometry against
`RegionOwnership.h`, so the property "the clamped point is owned by this server" is directly
assertable without a run, at both 2 and 4 server partitions.

### 5.4 Related: the partition changes topology on rebalance

Not a backlog item and not scheduled here, but it interacts with §5.3 and with any Phase D work at 4
servers, so it is recorded rather than left to be rediscovered.

The **initial** partition is a 2-D grid (§5.3). The **repartition** path emits 1-D X slices only —
`SystemManager.cpp:386-389` sets every region to the full Z extent, commented "Slices span the whole
Z extent. A 1-D split is all the forced-repartition flag needs to express". So the first rebalance on
a 4-server run silently reshapes a 2 × 2 grid into 4 vertical strips, changing every region at once
rather than moving one border.

E4 ran at 2 servers, where the initial partition is already 1-D, so the two topologies coincide and
this never bit. Any 4-server rebalancing measurement would hit it, and would be measuring a
whole-partition reshape rather than the incremental border movement the balancer is described as
performing.

### 5.5 Documentation to correct

This phase carries the largest documentation debt, because §5.3 is a defect that three separate
documents currently describe incorrectly.

| File | Claim to correct |
|---|---|
| `ServerWorldManager.cpp:2325-2326` | The function's own header comment: "The bounds mirror `IsObjectInBorder` exactly - half-open on X (>= min, < max), closed on Z (>= min, <= max)". False since the ownership unification. **This is the origin of the other two.** |
| `ServerWorldManager.cpp:2350` | "Z's upper bound is inclusive, so max is legal and needs no epsilon" — the inline justification for the defect itself. |
| `docs/SPATIAL-PARTITIONING.md:52-54` | Names the function `CalculateIncomingObjectOffsetedPosition` (no such name), cites `ServerWorldManager.cpp:296-314` (now `:2338`), and describes it as having "most of its branches currently commented out, leaving only a Z-axis floor adjustment active" — the pre-rewrite state. |
| `CLAUDE.md` audit bullet for the offset function | Corrected on 2026-08-23 to say the body is wrong; must be corrected *again* once §5.3 fixes it, to say what it now does. |
| `docs/EVALUATION.md` §7 item 7 | Closes when the function is fixed and wired in. |
| `docs/EVALUATION.md` §7 item 2 and §6 | The conditional ownership guarantee — "the atomicity guarantee stays conditional on `--handoff-lookahead > 0`, the non-default case" — changes meaning when that becomes the default. |
| `docs/SPATIAL-PARTITIONING.md` | Add the partition-topology discontinuity (§5.4): the initial partition is a 2-D grid, repartitioning emits 1-D X slices. Nothing currently documents that they differ. |

### 5.6 Gate

The no-op gate applies in an unusual form here, because the point of the change *is* to alter the
default. Gate on the **old** default instead: at an explicitly passed `--handoff-lookahead 0`, a
paced run must still reproduce the pre-change baseline on §1.3's stable field set. That separates "the new default
behaves differently" (intended) from "the old path changed" (a defect).

Additionally, `analyse.py`'s `check_custody` reports a non-zero `hoCustody` at exit as an invariant
failure, and that check must not be weakened to make these runs pass. A run intended to pass the gate
is drained until `hoCustody` reads 0.

### 5.7 Re-runs

Full sweep, as approved:

| Experiment | Why it must be re-run |
|---|---|
| E4 | Conservation-sensitive; the rebalancing regression lives here |
| E7 | Conservation-sensitive; ownership gaps become near-permanent past the budget |
| E1 | Regression — locality should be untouched, and that should be shown, not assumed |
| E2 | Regression — border crossings are the halo's acceptance test, which handoff timing touches |

E3, E6 and E8 are not conservation-sensitive and do not need re-running for this phase, provided the
Phase D gate (§5.6) passes.

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

Every phase begins with step 0 from §1.1: build HEAD and generate the baseline its gate compares
against, because no stored dataset survives.

| Phase | Items closed | Can alter a result? | Re-runs | Gate |
|---|---|---|---|---|
| A — instrumentation & harness | 10, 11, E3 caveat, E8 caveat | No | E8 (×2 client counts), E3, epoch-align on/off | Paced run reproduces step-0 baseline exactly |
| B — attribute E4 regression | 12 | No (no production code) | 1 pre-check; **only if it survives**, 4 points × E4 | None |
| C — latency & generalised bound | §5 latency, AP §6 finding | Default-off | E5, partial sweep | Zero-latency no-op + arithmetic identity |
| D — ownership | 2, 7, §6 conditional guarantee | Yes, by design | E1, E2, E4, E7 | Old default reproduces baseline exactly |

The ordering is load-bearing in two places: **B before C and D**, because a bisect window only grows;
and **C before D**, because latency is what makes the ownership fix measurable. A and B can be worked
concurrently — A touches instrumentation and the harness, B touches neither.

### 7.1 What the assessment pass changed

This document was revised after verifying each item against the source rather than against the
documentation describing it. Five things moved:

| Finding | Effect on the plan |
|---|---|
| `runs/` is absent — no dataset was ever committed (§1.1) | Every phase gains a step 0; re-analysis is no longer an option anywhere |
| `totalSentData` is post-coalescing socket bytes (§2.1) | Item 10 removes the overhead model instead of refining it — the strongest value-per-effort item on the list |
| `--epoch-align-us` exists and no experiment used it (§2.3) | New Phase A experiment costing runs and no code |
| E4's loss may be drain truncation, never checked (§3.0) | Phase B demoted behind a one-run pre-check that can delete it entirely |
| The clamp's Z bound contradicts `OwningServerFor` (§5.3) | Item 7 becomes "fix, then wire in"; it is a live defect masked only by the result being discarded, and reachable at 4 servers |
| `HALO_STALE_TICKS = 30` caps injectable latency at ~50 ms (§4.2) | Promoted from a work-list bullet to Phase C's first task |

Two of these — the missing baselines and the stale clamp bounds — contradict statements in
`docs/EVALUATION.md` and `CLAUDE.md` respectively. Both of those documents should be corrected as
part of the phase that acts on them, not separately.
