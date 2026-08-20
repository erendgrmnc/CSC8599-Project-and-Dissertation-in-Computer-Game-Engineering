# Handoff custody: making ownership transfer lossless (2026-08-20)

Date: 2026-08-20
Status: **design — approved, not yet implemented**
Closes: `docs/EVALUATION.md` backlog items 1, 2, 6, 7 ("Batch B")
Follows: `2026-08-19-evidence-completion-design.md`, and Batch A (commits `fd98f97..02e306b`)

## 0. What exploration changed

The backlog described four defects. Three of them are not what they said.

- **Items 1 and 2 are one problem**, and the ack that would fix it is already on the wire.
  `DistributedGameServerManager.cpp:410` sends `StartSimulatingObjectReceivedPacket` whenever the
  receiver accepts a transfer. Both the sender-side handlers are empty bodies
  (`DistributedGameServerManager.cpp:578`, `ServerWorldManager.cpp:1673`), as is
  `NetworkObject::OnTransitionHandshakeReceived`. The protocol exists; nobody listens.
- **Item 6 is not a defect.** `TakeLoadReport` states the reason it buckets object counts rather than
  contacts: a contact belongs to two objects that may fall in different buckets, so charging it to one
  is arbitrary. That is a deliberate tradeoff, and it is being *restated as a limitation*, not changed.
- **Item 7 is probably unreachable.** Since ownership was unified behind `OwningServerFor()`, a handoff
  target is computed from the transmitted position, so an incoming object is inside the receiving
  region by construction and `CalculateIncomingObjectOffsetPosition` would be a no-op. This is
  *measured* rather than asserted (§4).

**And one thing the docs get wrong.** `CLAUDE.md` says the ownership gap "is closed: the sender no
longer releases on send, it releases on the same `senderTick + lookahead` the receiver installs on."
That is true only when `--handoff-lookahead > 0`. The default is **0**, and at 0
`ScheduleOutgoingObject` calls `HandleOutgoingObject` immediately — release on send, gap wide open.
E3, E5's sweeps, E6 and E7 all ran at 0; only E2, E4 and E1 ran at 300. The atomicity guarantee is
opt-in behind a non-default flag, and that needs saying plainly.

## 1. The actual bug

`HandleOutgoingObject` does not deactivate the outgoing object. It **tears it down and erases the pool
entry**, deliberately — leaving a deactivated twin behind is what used to make per-server state
O(world). After it runs, the only trace of the object on the sender is `mLastKnownOwner`.

So if the receiver never installs the object, it exists nowhere. And that is reachable without any
packet loss at all:

```cpp
if (mServerWorldManager->StartHandlingObject(packet)) {
    SendTransactionHandshakePacket(packet->senderServerID, packet->objectID);
} else {
    std::cout << "Failed to receive object " << ... << std::endl;   // no ack, no retry, object gone
}
```

Transfers and acks both go over `SendReliablePacket`, so ordinary loss is already covered by ENet
retransmission. The real exposures are a receiver that *refuses* the object, and a peer that dies mid
transfer. E7 saw the consequence at scale: past ~3,384 objects/server, `conservation_delta` reached
−4 to −8.

## 2. Design: retain custody, do not delay release

The obvious fix — hold the release until the ack arrives — is wrong, and the reason matters.

At `--handoff-lookahead 0` the receiver applies on arrival. If the sender also waited for the ack it
would keep simulating until roughly one RTT *after* the receiver started, so the two servers would
both simulate the object. That converts an ownership **gap** into an ownership **overlap**: still a
violation of "exactly one owner", but now with two integrators producing divergent state. Trading a
gap for a double-simulation is not an improvement.

Instead, separate two things the code currently conflates:

| | meaning | when it ends |
|---|---|---|
| **Simulation** | this server integrates the object | at release — **unchanged** |
| **Custody** | this server is *responsible* for the object arriving somewhere | at ack |

`HandleOutgoingObject` keeps behaving exactly as it does today, at every lookahead. What is added is
that the sender does not *forget* the transfer until it is acknowledged.

### Mechanism

- **`mPendingTransfers`**: `map<objectID, PendingTransfer>` where `PendingTransfer` holds a copy of the
  `StartSimulatingObjectPacket`, the target server id, the tick it was last sent on, and an attempt
  count. The packet copy is the point: it fully describes the object, so nothing else needs retaining.
- **Recorded** by `DistributedGameServerManager` immediately after the send at `:833`, via a new
  `ServerWorldManager::RecordPendingTransfer(const StartSimulatingObjectPacket&)`.
- **Discharged** by `HandleTransitionHandshakeReceived`, which erases the entry and calls
  `NetworkObject::OnTransitionHandshakeReceived()` so `mIsWaitingHandshake` finally clears.
- **`FlushPendingTransfers()`**, once per tick:
  - acked → already erased, nothing to do;
  - `tick - lastSentTick >= retryTicks` and attempts remain → queue a resend, bump the attempt count;
  - attempts exhausted → **reclaim**.

### Reclaim

Reclaiming is `ApplyIncomingObject(storedPacket)` — the sender applies its own transfer packet back to
itself. This is the same code path an incoming handoff uses, so it needs no new construction logic, and
it is correct precisely because `HandleOutgoingObject` *erased* rather than nulled the pool entry: a
fresh construct is the normal arrival case, not a tombstone conflict. `mLastKnownOwner` for the id must
be cleared at the same time, or commands would keep being forwarded to a server that does not have it.

### Layering

`ServerWorldManager` must not see the network layer. It already has the idiom: `mPendingRelays` is a
vector the world manager pushes to and `DistributedGameServerManager` drains via `TakePendingRelay`
(`ServerWorldManager.cpp:612`). Resends reuse it exactly — `TakeHandoffResend(StartSimulatingObjectPacket&)`
hands back a copy, and the manager sends it through the existing path.

### Why this preserves every existing measurement

Nothing observable changes until an ack fails to arrive. Simulation ownership transfers on the same
tick it does today, at every lookahead. In a healthy run `mPendingTransfers` fills and empties within
about one RTT and no other code reads it. E1–E8 therefore remain valid, and §5 states how that is
verified rather than assumed.

## 3. Configuration

`--handoff-retry-ticks N` (default 30, `0` disables retry and restores exactly today's behaviour so
the two can be compared) and `--handoff-max-attempts N` (default 3).

**Both must be added in two places** — parsed in `DistributedGameServer/ServerStarter.cpp` *and*
forwarded in `PhysicsServerMidware/ProgramStart.cpp`. A game-server flag the midware does not forward
is silently ignored with no error; that is how `--drain-seconds` sat dead long enough to invalidate E8,
and it is the single most repeated mistake in this codebase.

They must also reach `tools/measure.ps1` and `tools/run-experiments.ps1`, or the flag is unreachable
from the harness — the *second* half of that same mistake, found in Batch A.

## 4. Item 7, measured rather than argued

Add `hoClamp`, incremented in `ApplyIncomingObject` when
`CalculateIncomingObjectOffsetPosition(pos) != pos` — i.e. when an incoming object actually lands
outside the receiving region. **The clamp is computed and discarded, never applied**, so this is pure
observation with no behavioural effect.

If `hoClamp` is 0 across the whole re-measurement suite, item 7 closes as unreachable *with evidence*.
If it is non-zero, wiring the function up becomes a real task with a real justification, which is more
than it has today.

## 5. Validation

In order, because each stage gates the next:

1. **Unit tests first.** The release/retry/reclaim decision factors into a pure function over
   `(acked, attempts, lastSentTick, tick, retryTicks, maxAttempts)` returning
   `Wait | Resend | Reclaim`. That is where the logic lives and it belongs in
   `tools/InteractionTests` under the existing `TEST`/`CHECK` harness. TDD applies here.
2. **No-op verification.** Two lookaheads are in play and they are different flags — `--handoff-lookahead`
   governs ownership release, `--halo-lookahead` governs shadow extrapolation. The two runs are chosen
   to cover both *handoff* regimes:

   | run | `--handoff-lookahead` | `--halo-lookahead` | must reproduce |
   |---|---|---|---|
   | E2 (`headon`, halo widths 0 and 8, 3 repeats) | **300** (scheduled release) | 4 | crossings 100/0; contacts 76,600/82,390 |
   | E5 L=24 knee (halo widths 4-7, 2 repeats) | **0** (release on send) | 24 | knee at halo width 6 |

   Both must reproduce **exactly**. This is the check that caught my incorrect no-op claim in Batch A.
3. **The payoff.** E7 at 8,000 objects, 2 servers, halo on. `conservation_delta` must go from −4/−8 to
   **0**, with `hoAbandon` reporting how many transfers had to be reclaimed. A non-zero `hoAbandon`
   with zero loss is a success, not a failure: it is the mechanism working.
4. **E4's rebalancing case.** Gap ticks may remain — the gap is a timing property and this design does
   not claim to remove it — but nothing may be lost.
5. **`hoClamp`** read across all of the above for §4.

## 6. Out of scope

- **Removing the ownership gap.** Custody retention makes transfer *lossless*, not *instantaneous*.
  Exactly-one-owner at every instant needs either a shared clock (which `--handoff-lookahead > 0`
  approximates) or a barrier (rejected — it couples every server to the slowest). The gap stays a
  stated conditional guarantee.
- **A NACK.** The receiver could reject explicitly instead of letting the sender time out. Cheap, but
  it is a wire-format change for a latency improvement on a path that should be rare.
- **Item 6.** Documentation only.
