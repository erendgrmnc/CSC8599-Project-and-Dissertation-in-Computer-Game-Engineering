# Handoff Custody Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make object handoff lossless — a sender that transfers an object keeps the transfer packet until the receiver acknowledges, resends on timeout, and reclaims the object if the transfer never lands.

**Architecture:** Separate *simulation ownership* (unchanged — `HandleOutgoingObject` still tears the object down on exactly the tick it does today) from *custody* (new — the sender stays responsible until acked). All decision logic lives in one pure function so it can be unit-tested; everything else is mechanical wiring verified by re-measurement.

**Tech Stack:** C++20, MSVC x64, CMake-generated VS solution. ENet networking, all packets strict POD. Tests via the repo's dependency-free `TEST`/`CHECK` harness in `tools/InteractionTests`. Experiment harness is PowerShell (`tools/measure.ps1`, `tools/run-experiments.ps1`) plus `tools/analyse.py` (stdlib only).

**Spec:** `docs/superpowers/specs/2026-08-20-handoff-custody-design.md`

## Global Constraints

- **Build Release, x64.** MSBuild lives at `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe`. Build all three roles together: `/t:EntryPointManager;EntryPointMidware;EntryPointServer`. The `InteractionTests` target is under a solution folder: `/t:Tools\InteractionTests`.
- **A new game-server flag must be added in THREE places or it is silently ignored:** parsed in `DistributedGameServer/ServerStarter.cpp`, forwarded in `PhysicsServerMidware/ProgramStart.cpp`, and exposed in `tools/measure.ps1` **and** `tools/run-experiments.ps1`. This has been the single most repeated defect in this codebase.
- **`-Values` in `run-experiments.ps1` is a comma-separated STRING, not an array.** `powershell -File` cannot parse array arguments; `-Values 1,2` arrives as `12`.
- **Every measurement run needs `--fixed-step` and `--seed`** or the numbers are not comparable. The harness passes both.
- **Simulation behaviour must not change on the healthy path.** Task 6 verifies this by exact reproduction, not by assertion.
- **Commit style:** short, understandable messages, small batches, **no co-author trailers and no `Claude-Session` lines**.
- **`ServerWorldManager` must not see the network layer.** It queues work; `DistributedGameServerManager` drains it. The existing idiom is `PendingRelay` / `PopPendingRelay` (`ServerWorldManager.cpp:615`).
- **Pure, shared logic goes in `CSC8503CoreClasses/DistributedSystemCommonFiles/`** — the precedent is `RegionOwnership.h` and `NetworkIdSpace.h`. These headers carry no `USEGL` / `DISTRIBUTEDSYSTEMACTIVE` guards because servers, client and the test target all include them.
- **New source files must be added to the relevant `CMake*.cmake` / `CMakeLists.txt`**, not just to disk.

## File Structure

| File | Responsibility |
|---|---|
| `CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h` | **New.** The pure custody decision: given tick, last-sent tick, attempt count and config, return Wait / Resend / Reclaim. Header-only, no dependencies. |
| `tools/InteractionTests/HandoffCustodyTests.cpp` | **New.** Unit tests for the above. |
| `tools/InteractionTests/CMakeLists.txt` | Register the new test file. |
| `DistributedGameServer/ServerWorldManager.h/.cpp` | Holds `mPendingTransfers`, records and discharges them, runs `FlushPendingTransfers` each tick, queues resends, reclaims on exhaustion, owns the new counters. |
| `DistributedGameServer/DistributedGameServerManager.h/.cpp` | Records a pending transfer after the send; drains the resend queue; routes the ack packet to the world manager. |
| `DistributedGameServer/ServerStarter.cpp` | Parses the two new flags; emits new counters on `@@FINAL`. |
| `PhysicsServerMidware/ProgramStart.cpp` | Forwards the two new flags. |
| `tools/measure.ps1`, `tools/run-experiments.ps1` | Expose the two new flags. |
| `tools/analyse.py` | Reads the new counters; fails a run that loses an object. |
| `docs/EVALUATION.md`, `CLAUDE.md` | Item 6 restated as a limitation; the lookahead-0 correction. |

---

### Task 1: The custody decision, as a pure function

All the logic that can be wrong lives here. Everything in later tasks is wiring.

**Files:**
- Create: `CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h`
- Create: `tools/InteractionTests/HandoffCustodyTests.cpp`
- Modify: `tools/InteractionTests/CMakeLists.txt:19-32`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class NCL::Distributed::CustodyAction { Wait, Resend, Reclaim }`
  - `struct NCL::Distributed::CustodyConfig { int retryTicks = 30; int maxAttempts = 3; }`
  - `CustodyAction NCL::Distributed::DecideCustody(uint64_t currentTick, uint64_t lastSentTick, int attempts, const CustodyConfig& config)`

- [ ] **Step 1: Write the failing test**

Create `tools/InteractionTests/HandoffCustodyTests.cpp`:

```cpp
// The custody decision for an unacknowledged handoff.
//
// Acknowledged transfers are ERASED from mPendingTransfers, so they never reach this
// function - it only ever sees transfers still outstanding. That is what keeps it a
// pure function of (tick, lastSentTick, attempts, config) with no "acked" input.

#include "TestHarness.h"

#include "DistributedSystemCommonFiles/HandoffCustody.h"

using namespace NCL::Distributed;

TEST(CustodyWaitsBeforeTheRetryTimeout) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	// Sent on tick 100, now tick 129: 29 ticks elapsed, one short of the timeout.
	CHECK(DecideCustody(129, 100, 1, config) == CustodyAction::Wait);
}

TEST(CustodyResendsExactlyOnTheTimeoutTick) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	// Boundary: elapsed == retryTicks must resend, not wait one more tick.
	CHECK(DecideCustody(130, 100, 1, config) == CustodyAction::Resend);
}

TEST(CustodyResendsWhileAttemptsRemain) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	CHECK(DecideCustody(500, 100, 2, config) == CustodyAction::Resend);
}

TEST(CustodyReclaimsWhenAttemptsAreExhausted) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	// attempts == maxAttempts means all three sends have happened and none was acked.
	CHECK(DecideCustody(500, 100, 3, config) == CustodyAction::Reclaim);
	CHECK(DecideCustody(500, 100, 4, config) == CustodyAction::Reclaim);
}

TEST(CustodyReclaimsOnFirstTimeoutWhenOnlyOneAttemptIsAllowed) {
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 1;
	CHECK(DecideCustody(130, 100, 1, config) == CustodyAction::Reclaim);
}

TEST(CustodyDisabledByZeroRetryTicksNeverActs) {
	// retryTicks 0 restores exactly the pre-custody behaviour, so the two can be
	// compared in one experiment. It must never resend AND never reclaim, however
	// long the transfer has been outstanding.
	CustodyConfig config;
	config.retryTicks = 0;
	config.maxAttempts = 3;
	CHECK(DecideCustody(100000, 100, 1, config) == CustodyAction::Wait);
	CHECK(DecideCustody(100000, 100, 99, config) == CustodyAction::Wait);
}

TEST(CustodyWaitsIfTheTickCounterIsBehindTheSendTick) {
	// Defensive: a repartition or a reset must not produce a huge unsigned elapsed
	// value and trigger an instant reclaim.
	CustodyConfig config;
	config.retryTicks = 30;
	config.maxAttempts = 3;
	CHECK(DecideCustody(50, 100, 1, config) == CustodyAction::Wait);
}
```

Register it in `tools/InteractionTests/CMakeLists.txt` by adding `"HandoffCustodyTests.cpp"` to the `add_executable` source list, after `"PacketSizeTests.cpp"`.

- [ ] **Step 2: Run test to verify it fails**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
```

Expected: **compile error** — `Cannot open include file: 'DistributedSystemCommonFiles/HandoffCustody.h'`.

- [ ] **Step 3: Write minimal implementation**

Create `CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h`:

```cpp
#pragma once
#include <cstdint>

// When a server hands an object to a neighbour it tears its own copy down - the pool
// entry is ERASED, not nulled - so if the receiver never installs the object it exists
// nowhere. Transfers and acks both go over reliable ENet, so ordinary packet loss is
// already covered; the exposures are a receiver that REFUSES the object
// (StartHandlingObject returning false, which acks nothing) and a peer that dies
// mid-transfer.
//
// The sender therefore keeps the transfer packet until the receiver acknowledges it.
// This decides what to do with one still-unacknowledged transfer. Acknowledged ones
// are erased on arrival of the ack and never reach here.
//
// Deliberately NOT a decision about when to release the object. Releasing late would
// mean the sender kept simulating an object the receiver had already started
// simulating, turning an ownership gap into an ownership OVERLAP with two integrators
// diverging - strictly worse than the sub-millisecond freeze a gap causes. Release
// timing is unchanged; only the retry obligation is new.
namespace NCL::Distributed {

	enum class CustodyAction {
		Wait,       // still within the retry window
		Resend,     // timed out, attempts remain
		Reclaim,    // timed out, attempts exhausted - take the object back
	};

	struct CustodyConfig {
		// Ticks to wait for an ack before resending. 0 disables retry entirely and
		// restores the original behaviour, so an experiment can compare the two.
		int retryTicks = 30;
		// Total sends allowed, including the first. 1 means reclaim on first timeout.
		int maxAttempts = 3;
	};

	inline CustodyAction DecideCustody(uint64_t currentTick, uint64_t lastSentTick,
		int attempts, const CustodyConfig& config) {
		if (config.retryTicks <= 0) {
			return CustodyAction::Wait;
		}
		// The tick counter can sit below lastSentTick after a repartition. Unsigned
		// subtraction would wrap to an enormous elapsed value and reclaim instantly.
		if (currentTick < lastSentTick) {
			return CustodyAction::Wait;
		}
		if ((currentTick - lastSentTick) < static_cast<uint64_t>(config.retryTicks)) {
			return CustodyAction::Wait;
		}
		if (attempts >= config.maxAttempts) {
			return CustodyAction::Reclaim;
		}
		return CustodyAction::Resend;
	}
}
```

- [ ] **Step 4: Run tests to verify they pass**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
```

Expected: `94 passed, 0 failed.` (87 existing + 7 new) and exit code 0.

- [ ] **Step 5: Commit**

```bash
git add CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h tools/InteractionTests/HandoffCustodyTests.cpp tools/InteractionTests/CMakeLists.txt
git commit -m "feat(handoff): pure custody decision for unacked transfers"
```

---

### Task 2: Hold custody in ServerWorldManager

Mechanical wiring. The decision logic is Task 1's, already tested; this task's correctness is verified by Task 6's re-measurement.

**Files:**
- Modify: `DistributedGameServer/ServerWorldManager.h` (near `ScheduledHandoff`, `:488-493`)
- Modify: `DistributedGameServer/ServerWorldManager.cpp` (`HandleTransitionHandshakeReceived` at `:1673`; `Update` — insert after the `FlushScheduledReleases()` call)

**Interfaces:**
- Consumes: `NCL::Distributed::DecideCustody`, `CustodyAction`, `CustodyConfig` (Task 1).
- Produces, all on `ServerWorldManager`:
  - `void RecordPendingTransfer(const CSC8503::StartSimulatingObjectPacket& packet, int targetServerID)`
  - `bool PopHandoffResend(CSC8503::StartSimulatingObjectPacket& out)`
  - `void SetCustodyConfig(int retryTicks, int maxAttempts)`
  - `int GetHandoffsResent() const`, `int GetHandoffsReclaimed() const`, `int GetPendingCustodyCount() const`

- [ ] **Step 1: Add the state to the header**

In `DistributedGameServer/ServerWorldManager.h`, immediately after the `ScheduledHandoff` block (`:488-493`), add:

```cpp
			// A transfer that has been SENT but not yet acknowledged.
			//
			// Simulation ownership is unaffected: HandleOutgoingObject still tears the
			// object down on exactly the tick it always did. What this adds is that the
			// sender does not FORGET the transfer. HandleOutgoingObject erases the pool
			// entry, so without this record an object the receiver never installed
			// exists nowhere - which is how E7 lost objects past the tick budget.
			//
			// unique_ptr rather than by value because this header only forward-declares
			// StartSimulatingObjectPacket, matching ScheduledHandoff above.
			struct PendingTransfer {
				std::unique_ptr<CSC8503::StartSimulatingObjectPacket> packet;
				int targetServerID = -1;
				uint64_t lastSentTick = 0;
				int attempts = 1;
			};
			// Keyed by object id: a resend must replace, never duplicate, the record.
			std::map<int, PendingTransfer> mPendingTransfers;
			void FlushPendingTransfers();
			// Object ids whose transfer needs re-sending. Drained by
			// DistributedGameServerManager, which owns the network layer.
			std::vector<int> mHandoffResendQueue;
			NCL::Distributed::CustodyConfig mCustodyConfig;
			int mHandoffsResent = 0;
			int mHandoffsReclaimed = 0;
```

Add the public accessors near the other handoff accessors (`:341-355`):

```cpp
			void RecordPendingTransfer(const CSC8503::StartSimulatingObjectPacket& packet,
				int targetServerID);
			bool PopHandoffResend(CSC8503::StartSimulatingObjectPacket& out);
			void SetCustodyConfig(int retryTicks, int maxAttempts) {
				mCustodyConfig.retryTicks = retryTicks;
				mCustodyConfig.maxAttempts = maxAttempts;
			}
			int GetHandoffsResent() const {
				return mHandoffsResent;
			}
			int GetHandoffsReclaimed() const {
				return mHandoffsReclaimed;
			}
			int GetPendingCustodyCount() const {
				return static_cast<int>(mPendingTransfers.size());
			}
```

Add `#include "DistributedSystemCommonFiles/HandoffCustody.h"` to the include block at the top (`:8-11`), and `#include <map>` if not already present.

- [ ] **Step 2: Implement record, discharge and flush**

In `DistributedGameServer/ServerWorldManager.cpp`, replace the empty `HandleTransitionHandshakeReceived` (`:1673`) with:

```cpp
void DistributedGameServer::ServerWorldManager::HandleTransitionHandshakeReceived(
	CSC8503::StartSimulatingObjectReceivedPacket* packet) {
	if (packet == nullptr) {
		return;
	}
	// The ack discharges custody: the receiver has accepted the object and will
	// install it on the agreed tick, so this server is no longer responsible for it.
	mPendingTransfers.erase(packet->objectID);

	// Clearing mIsWaitingHandshake is only possible while the NetworkObject still
	// exists, and whether it does depends on the handoff lookahead:
	//
	//  - lookahead 0: the object was released - and TORN DOWN - on the tick it was
	//    sent, so by the time this ack arrives there is nothing left to call. Calling
	//    NetworkObject::OnTransitionHandshakeReceived() unguarded here would be a
	//    use-after-free.
	//  - lookahead > 0: release happens at senderTick + lookahead, far later than one
	//    round trip, so the object is still here and the flag can be cleared.
	//
	// Looking it up rather than assuming either case is what makes this safe in both.
	const auto poolEntry = mCreatedObjectPool.find(packet->objectID);
	if (poolEntry != mCreatedObjectPool.end() && poolEntry->second != nullptr) {
		if (auto* networkObject = poolEntry->second->GetNetworkObject()) {
			networkObject->OnTransitionHandshakeReceived();
		}
	}
}
```

Add, next to `FlushScheduledReleases`:

```cpp
void DistributedGameServer::ServerWorldManager::RecordPendingTransfer(
	const CSC8503::StartSimulatingObjectPacket& packet, int targetServerID) {
	PendingTransfer transfer;
	transfer.packet = std::make_unique<CSC8503::StartSimulatingObjectPacket>(packet);
	transfer.targetServerID = targetServerID;
	transfer.lastSentTick = mTickCounter;
	transfer.attempts = 1;
	mPendingTransfers[packet.objectID] = std::move(transfer);
}

bool DistributedGameServer::ServerWorldManager::PopHandoffResend(
	CSC8503::StartSimulatingObjectPacket& out) {
	while (!mHandoffResendQueue.empty()) {
		const int objectID = mHandoffResendQueue.front();
		mHandoffResendQueue.erase(mHandoffResendQueue.begin());
		const auto entry = mPendingTransfers.find(objectID);
		// Acked between being queued and being drained - nothing to resend.
		if (entry == mPendingTransfers.end() || entry->second.packet == nullptr) {
			continue;
		}
		out = *entry->second.packet;
		return true;
	}
	return false;
}

void DistributedGameServer::ServerWorldManager::FlushPendingTransfers() {
	if (mPendingTransfers.empty()) {
		return;
	}
	// std::map, so object-id order - two reclaims on the same tick always happen in
	// the same order, which is what keeps a run reproducible.
	for (auto entry = mPendingTransfers.begin(); entry != mPendingTransfers.end(); ) {
		const NCL::Distributed::CustodyAction action = NCL::Distributed::DecideCustody(
			mTickCounter, entry->second.lastSentTick, entry->second.attempts,
			mCustodyConfig);

		if (action == NCL::Distributed::CustodyAction::Wait) {
			++entry;
			continue;
		}

		if (action == NCL::Distributed::CustodyAction::Resend) {
			mHandoffResendQueue.push_back(entry->first);
			entry->second.lastSentTick = mTickCounter;
			++entry->second.attempts;
			++mHandoffsResent;
			++entry;
			continue;
		}

		// Reclaim. The sender applies its OWN transfer packet back to itself, which is
		// the same path an incoming handoff takes - correct precisely because
		// HandleOutgoingObject erased the pool entry rather than nulling it, so a
		// fresh construct is the normal arrival case and not a tombstone conflict.
		const int objectID = entry->first;
		CSC8503::StartSimulatingObjectPacket reclaimed = *entry->second.packet;
		reclaimed.newOwnerServerID = mServerID;
		entry = mPendingTransfers.erase(entry);

		// Cleared BEFORE the apply: while this entry stands, a command for the object
		// would be forwarded to a server that does not have it.
		mLastKnownOwner.erase(objectID);
		mLastKnownOwnerTick.erase(objectID);

		ApplyIncomingObject(&reclaimed);
		++mHandoffsReclaimed;
		std::cout << "Reclaimed unacknowledged handoff of object " << objectID << "\n";
	}
}
```

- [ ] **Step 3: Call it once per tick**

In `ServerWorldManager::Update`, on the line immediately after the existing `FlushScheduledReleases();` call, add:

```cpp
	// After the releases, so a transfer scheduled and released this tick is recorded
	// before its retry window is ever evaluated.
	FlushPendingTransfers();
```

- [ ] **Step 4: Build and verify nothing regressed**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:EntryPointManager`;EntryPointMidware`;EntryPointServer /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
```

Expected: all three roles link; `94 passed, 0 failed.` Nothing records a pending transfer yet, so behaviour is unchanged — that is correct at this stage.

- [ ] **Step 5: Commit**

```bash
git add DistributedGameServer/ServerWorldManager.h DistributedGameServer/ServerWorldManager.cpp
git commit -m "feat(handoff): keep custody of a transfer until it is acked"
```

---

### Task 3: Wire the network side

**Files:**
- Modify: `DistributedGameServer/DistributedGameServerManager.cpp:578-587` (`HandleTransitionHandshakePacketReceived`), `:772-787` (`HandleObjectTransitions` send loop), and the tick body around `:174`

**Interfaces:**
- Consumes: `RecordPendingTransfer`, `PopHandoffResend` (Task 2); `SendPacketToServer(int, GamePacket&)` and `SendFinishTransactionPacket(NetworkObject&)` (existing).
- Produces: nothing new for later tasks.

- [ ] **Step 1: Route the ack to the world manager**

Replace the body of `HandleTransitionHandshakePacketReceived` (`:578`) — currently a loop with its only statement commented out — with:

```cpp
void DistributedGameServer::DistributedGameServerManager::HandleTransitionHandshakePacketReceived(
	StartSimulatingObjectReceivedPacket* packet) {
	if (packet == nullptr || mServerWorldManager == nullptr) {
		return;
	}
	mServerWorldManager->HandleTransitionHandshakeReceived(packet);
}
```

- [ ] **Step 2: Record custody after the send**

`SendFinishTransactionPacket` builds the packet locally, so the send loop cannot see it. Change `SendFinishTransactionPacket` to hand it back. In `DistributedGameServerManager.h`, change the declaration to:

```cpp
			bool SendFinishTransactionPacket(NetworkObject& obj,
				StartSimulatingObjectPacket& outSent) const;
```

In `DistributedGameServerManager.cpp`, in `SendFinishTransactionPacket`, immediately before the existing `if (!SendPacketToServer(packet.newOwnerServerID, packet))` at `:862`, add:

```cpp
	outSent = packet;
```

Then in the `HandleObjectTransitions` send loop (`:775`), replace:

```cpp
		if (!SendFinishTransactionPacket(*networkObj)) {
			continue;
		}
```

with:

```cpp
		StartSimulatingObjectPacket sentPacket(0, 0, 0, NetworkState(), *networkObj->GetGameObject().GetPhysicsObject());
		if (!SendFinishTransactionPacket(*networkObj, sentPacket)) {
			continue;
		}
		// Custody starts here, not at release. The object is released on its normal
		// tick below; this record is what lets an unacknowledged transfer be resent
		// and, failing that, reclaimed.
		mServerWorldManager->RecordPendingTransfer(sentPacket, networkObj->GetNewServerID());
```

- [ ] **Step 3: Drain the resend queue each tick**

In `UpdateGameServerManager`, immediately after the existing `PublishHaloBand();` call, add:

```cpp
		// Resends for transfers that were never acknowledged. Same reliable path as
		// the original send; the receiver dedupes by object id because an object it
		// already holds fails IsReleasePending and is ignored.
		StartSimulatingObjectPacket resend(0, 0, 0, NetworkState(), *(PhysicsObject*)nullptr);
		while (mServerWorldManager->PopHandoffResend(resend)) {
			SendPacketToServer(resend.newOwnerServerID, resend);
		}
```

> **Implementer note.** The two `StartSimulatingObjectPacket` scratch instances above are awkward: the packet has no default constructor, and the one in Step 3 dereferences a null `PhysicsObject*`, which is undefined behaviour and **must not ship**. Add a defaulted constructor to the struct instead:
>
> In `CSC8503CoreClasses/NetworkObject.h`, inside `struct StartSimulatingObjectPacket`, add alongside the existing constructor:
> ```cpp
> 		// Needed so callers can declare an output parameter to fill. The packet is
> 		// POD on the wire, so a zeroed instance is safe.
> 		StartSimulatingObjectPacket();
> ```
> In `CSC8503CoreClasses/NetworkObject.cpp`, next to the existing constructor:
> ```cpp
> StartSimulatingObjectPacket::StartSimulatingObjectPacket() {
> 	type = BasicNetworkMessages::StartSimulatingObjectInServer;
> 	size = sizeof(StartSimulatingObjectPacket) - sizeof(GamePacket);
> 	objectID = -1;
> 	newOwnerServerID = -1;
> 	senderServerID = -1;
> 	mControllerPlayerID = -1;
> }
> ```
> Then both scratch declarations become plain `StartSimulatingObjectPacket sentPacket;` and `StartSimulatingObjectPacket resend;`. Confirm `size` matches the existing constructor's convention by reading `NetworkObject.cpp` around the existing `StartSimulatingObjectPacket` constructor before copying the line above — if the existing one assigns `size = sizeof(StartSimulatingObjectPacket);` without subtracting, match that instead. The two must agree or the receiver reads the wrong length.

- [ ] **Step 4: Build and verify**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:EntryPointManager`;EntryPointMidware`;EntryPointServer /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
```

Expected: clean link, `94 passed, 0 failed.`

- [ ] **Step 5: Commit**

```bash
git add CSC8503CoreClasses/NetworkObject.h CSC8503CoreClasses/NetworkObject.cpp DistributedGameServer/DistributedGameServerManager.h DistributedGameServer/DistributedGameServerManager.cpp
git commit -m "feat(handoff): record custody on send, resend and ack on receipt"
```

---

### Task 4: Flags, through all four layers

**Files:**
- Modify: `DistributedGameServer/ServerStarter.cpp:113` (next to `SetHandoffLookaheadTicks`)
- Modify: `PhysicsServerMidware/ProgramStart.cpp` (the forwarding block, after the `--drain-seconds` entry)
- Modify: `tools/measure.ps1` (param block near `$DrainSeconds`, and the midware `-ArgumentList`)
- Modify: `tools/run-experiments.ps1` (param block near `$DrainSeconds`, manifest `fixed` block, and the `measure.ps1` invocation)

**Interfaces:**
- Consumes: `SetCustodyConfig(int, int)` (Task 2).
- Produces: `--handoff-retry-ticks`, `--handoff-max-attempts` reachable from `run-experiments.ps1` as `-HandoffRetryTicks`, `-HandoffMaxAttempts`.

- [ ] **Step 1: Parse in the game server**

In `DistributedGameServer/ServerStarter.cpp`, immediately after line 113 (`SetHandoffLookaheadTicks`), add:

```cpp
		// Custody: how long to wait for a handoff ack before resending, and how many
		// sends to attempt before taking the object back. 0 retry ticks disables the
		// mechanism and restores the pre-custody behaviour for comparison.
		worldManager->SetCustodyConfig(config.GetInt("--handoff-retry-ticks", 30),
			config.GetInt("--handoff-max-attempts", 3));
```

- [ ] **Step 2: Forward from the midware**

In `PhysicsServerMidware/ProgramStart.cpp`, after the `--drain-seconds` block, add:

```cpp
	if (config.Has("--handoff-retry-ticks")) {
		serverExtraArgs += " --handoff-retry-ticks " + std::to_string(config.GetInt("--handoff-retry-ticks", 30));
	}
	if (config.Has("--handoff-max-attempts")) {
		serverExtraArgs += " --handoff-max-attempts " + std::to_string(config.GetInt("--handoff-max-attempts", 3));
	}
```

- [ ] **Step 3: Expose in both PowerShell scripts**

In `tools/measure.ps1`, after the `[int]$DrainSeconds = -1,` parameter, add:

```powershell
    # Custody retry for unacknowledged handoffs. -1 leaves the server defaults alone.
    [int]$HandoffRetryTicks = -1,
    [int]$HandoffMaxAttempts = -1,
```

After the `$drainArg` line, add:

```powershell
$custodyArg = ""
if ($HandoffRetryTicks -ge 0) { $custodyArg += " --handoff-retry-ticks $HandoffRetryTicks" }
if ($HandoffMaxAttempts -ge 1) { $custodyArg += " --handoff-max-attempts $HandoffMaxAttempts" }
```

In the midware `-ArgumentList` string, insert `$custodyArg` immediately before `$bound`.

In `tools/run-experiments.ps1`, after `[int]$DrainSeconds = -1,` add:

```powershell
    [int]$HandoffRetryTicks = -1,
    [int]$HandoffMaxAttempts = -1,
```

Add to the manifest `fixed` block, after the `drainSeconds = $DrainSeconds` line:

```powershell
        handoffRetryTicks = $HandoffRetryTicks; handoffMaxAttempts = $HandoffMaxAttempts
```

And add to the `measure.ps1` invocation, on the line with `-DrainSeconds $DrainSeconds`:

```powershell
-HandoffRetryTicks $HandoffRetryTicks -HandoffMaxAttempts $HandoffMaxAttempts `
```

- [ ] **Step 4: Verify the flags actually reach the game server**

```powershell
$root = "C:\Users\erendegirmenci\Desktop\Projects\Distributed-Physics-Server-Simulation\tools"
foreach ($f in @("measure.ps1","run-experiments.ps1")) {
  $errs = $null
  [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root $f), [ref]$null, [ref]$errs) | Out-Null
  if ($errs.Count -eq 0) { "OK   $f" } else { "FAIL $f"; $errs | ForEach-Object { "     $($_.Message)" } }
}
```

Then rebuild all three roles, stage them, and run one short experiment with the flag set:

```powershell
Set-Location "C:\Users\erendegirmenci\Desktop\Projects\Distributed-Physics-Server-Simulation"
$built = "EntryPoint\Release"
$map = @{ "EntryPointManager" = "Manager"; "EntryPointMidware" = "Midware"; "EntryPointServer" = "DistributedPhysicsServer" }
foreach ($k in $map.Keys) { Copy-Item (Join-Path $built "$k.exe") (Join-Path "deploy\$($map[$k])" "EntryPoint.exe") -Force }
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 -Name custody-flagcheck -Sweep haloWidth -Values 8 -Repeats 1 `
  -Servers 2 -Objects 100 -Ticks 600 -Seed 42 -Workload headon -World "-150,150,-150,150" `
  -HaloLookahead 4 -HaloReliable -HandoffLookahead 300 -HandoffRetryTicks 30 -HandoffMaxAttempts 3
```

Then confirm the flag was forwarded, not silently dropped:

```bash
grep -o "Forwarding to spawned game servers:.*" runs/exp-custody-flagcheck/haloWidth8-r1/mid.log
```

Expected: the printed argument string **contains** `--handoff-retry-ticks 30 --handoff-max-attempts 3`. If it does not, the midware forwarding block is wrong — fix it before continuing, because every later task's measurements would be meaningless.

- [ ] **Step 5: Commit**

```bash
git add DistributedGameServer/ServerStarter.cpp PhysicsServerMidware/ProgramStart.cpp tools/measure.ps1 tools/run-experiments.ps1
git commit -m "feat(handoff): --handoff-retry-ticks and --handoff-max-attempts"
```

---

### Task 5: Telemetry, the hoClamp observation, and the analyse.py invariant

**Files:**
- Modify: `DistributedGameServer/ServerWorldManager.cpp` (`ApplyIncomingObject`)
- Modify: `DistributedGameServer/ServerWorldManager.h` (counter + accessor)
- Modify: `DistributedGameServer/ServerStarter.cpp` (the `@@FINAL` line, around `:307-311`)
- Modify: `tools/analyse.py`

**Interfaces:**
- Consumes: `GetHandoffsResent()`, `GetHandoffsReclaimed()`, `GetPendingCustodyCount()` (Task 2); `CalculateIncomingObjectOffsetPosition` (existing, `ServerWorldManager.cpp:1868`).
- Produces: `@@FINAL` fields `hoResent`, `hoReclaimed`, `hoCustody`, `hoClamp`.

- [ ] **Step 1: Add the hoClamp observation counter**

In `ServerWorldManager.h`, next to `mHandoffsReclaimed`, add:

```cpp
			// Incoming handoffs that landed OUTSIDE the receiving region.
			//
			// CalculateIncomingObjectOffsetPosition exists to nudge such an object
			// back inside but has never been called. Since ownership was unified
			// behind OwningServerFor(), a handoff target is computed from the
			// transmitted position, so an arrival should be in-region by
			// construction and the function should be dead code for a good reason
			// rather than by accident. This counts how often that is untrue. The
			// clamp is computed and DISCARDED - never applied - so this observes
			// without changing behaviour.
			int mHandoffsClamped = 0;
```

and the accessor beside the others:

```cpp
			int GetHandoffsClamped() const {
				return mHandoffsClamped;
			}
```

In `ApplyIncomingObject`, after the incoming position is known and before the object is installed, add:

```cpp
	if (mServerBorderData != nullptr) {
		const Maths::Vector3 incoming = packet->lastFullState.position;
		const Maths::Vector3 clamped = CalculateIncomingObjectOffsetPosition(incoming);
		if (clamped.x != incoming.x || clamped.z != incoming.z) {
			++mHandoffsClamped;
		}
	}
```

- [ ] **Step 2: Emit the four counters**

In `DistributedGameServer/ServerStarter.cpp`, in the `@@FINAL` chain next to `hoLate`, add:

Follow the existing pattern exactly: `GetServerWorldManager()` can return null on a server that never received its start packet — which is the very failure the `@@FINAL` line exists to make visible — so the surrounding code captures values into locals first and must not be called inline in the stream chain.

Next to the existing `int poolObjects = -1;` / `worldObjects` / `forwardEntries` / `haloObjects` declarations, add:

```cpp
		int handoffsResent = 0;
		int handoffsReclaimed = 0;
		int pendingCustody = 0;
		int handoffsClamped = 0;
```

Inside the existing `if (auto* worldManager = serverManager->GetServerWorldManager()) { ... }` block, alongside the existing assignments, add:

```cpp
			handoffsResent = worldManager->GetHandoffsResent();
			handoffsReclaimed = worldManager->GetHandoffsReclaimed();
			pendingCustody = worldManager->GetPendingCustodyCount();
			handoffsClamped = worldManager->GetHandoffsClamped();
```

Then in the `@@FINAL` stream chain, next to `hoLate`, add:

```cpp
			<< " hoResent=" << handoffsResent
			<< " hoReclaimed=" << handoffsReclaimed
			<< " hoCustody=" << pendingCustody
			<< " hoClamp=" << handoffsClamped
```

- [ ] **Step 3: Teach analyse.py the new invariant**

In `tools/analyse.py`, add a check that a run never loses an object even when transfers fail. Add this function next to the existing invariant checks:

```python
def check_custody(finals):
    """Custody invariant: reclaimed transfers must not lose objects.

    hoReclaimed > 0 is NOT a failure - it is the mechanism working. The failure is a
    non-zero conservation delta, which is checked separately. What is checked here is
    that nothing is left permanently in custody at the end of a run: a non-zero
    hoCustody means a transfer was still outstanding when the server exited, and that
    object is unaccounted for.
    """
    problems = []
    for row in finals:
        stranded = row.get("hoCustody", 0)
        if stranded:
            problems.append(
                "server %s ended with %d transfer(s) still in custody"
                % (row.get("id", "?"), stranded))
    return problems
```

Wire it into the same report the other invariant checks print to, and make a non-empty result contribute to the non-zero exit code.

- [ ] **Step 4: Build, test, and confirm the counters appear**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:EntryPointManager`;EntryPointMidware`;EntryPointServer /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
python -m unittest discover -s tools -p "test_analyse.py" -v
```

Expected: `94 passed, 0 failed.` and the existing `analyse.py` tests still pass.

- [ ] **Step 5: Commit**

```bash
git add DistributedGameServer/ServerWorldManager.h DistributedGameServer/ServerWorldManager.cpp DistributedGameServer/ServerStarter.cpp tools/analyse.py
git commit -m "feat(handoff): report custody counters and the hoClamp observation"
```

---

### Task 6: Re-measurement

This is where Tasks 2, 3 and 5 are actually verified. Nothing before this proves the wiring works.

**Files:**
- Create: `docs/superpowers/results/2026-08-20-B-custody.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the evidence Task 7's documentation edits cite.

- [ ] **Step 1: Rebuild and stage all three roles**

```powershell
Set-Location "C:\Users\erendegirmenci\Desktop\Projects\Distributed-Physics-Server-Simulation"
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:EntryPointManager`;EntryPointMidware`;EntryPointServer /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
$built = "EntryPoint\Release"
$map = @{ "EntryPointManager" = "Manager"; "EntryPointMidware" = "Midware"; "EntryPointServer" = "DistributedPhysicsServer" }
foreach ($k in $map.Keys) { Copy-Item (Join-Path $built "$k.exe") (Join-Path "deploy\$($map[$k])" "EntryPoint.exe") -Force }
```

- [ ] **Step 2: The no-op verification — both handoff-lookahead regimes**

```powershell
# E2: handoff lookahead 300 (scheduled release)
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 -Name custody-E2 -Sweep haloWidth -Values 0,8 -Repeats 3 `
  -Servers 2 -Objects 100 -Ticks 1800 -Seed 42 -Workload headon -World "-150,150,-150,150" `
  -HaloLookahead 4 -HaloReliable -HandoffLookahead 300

# E5 L=24 knee: handoff lookahead 0 (release on send)
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 -Name custody-E5L24 -Sweep haloWidth -Values 4,5,6,7 -Repeats 2 `
  -Servers 2 -Objects 100 -Ticks 1800 -Seed 42 -Workload headon -World "-150,150,-150,150" `
  -HaloLookahead 24 -HaloReliable -HandoffLookahead 0
```

**Required results — these are pass/fail gates, not observations:**

| run | must equal |
|---|---|
| `custody-E2` halo width 0 | crossings 100, contacts 76,600, all 3 repeats |
| `custody-E2` halo width 8 | crossings 0, contacts 82,390, all 3 repeats |
| `custody-E5L24` halo widths 4, 5 | crossings 100 |
| `custody-E5L24` halo widths 6, 7 | crossings 0 (knee at 6) |

If any differ, **stop**. The claim that custody is invisible on the healthy path is false and the design needs revisiting before any further measurement.

- [ ] **Step 3: The payoff — E7 at the capacity limit**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 -Name custody-E7 -Sweep objects -Values 4000,8000 -Repeats 3 `
  -Servers 2 -Ticks 1800 -Seed 42 -Workload uniform -World "-150,150,-150,150" `
  -HaloWidth 8 -HaloLookahead 4 -HaloReliable -HandoffLookahead 0
```

Compare against the recorded baseline in `runs/exp-capacity-halo`. **The result that matters: `conservation_delta` at 8,000 objects must be 0**, where the baseline was −4 to −8. A non-zero `hoReclaimed` alongside it is the mechanism working and should be reported, not treated as a failure.

- [ ] **Step 4: E4's rebalancing case, and read hoClamp everywhere**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 -Name custody-E4 -Sweep rebalanceInterval -Values 0,400 -Repeats 3 `
  -Servers 2 -Objects 4000 -Ticks 7200 -Seed 42 -Workload cluster -World "-150,150,-150,150" -HandoffLookahead 300
```

Gap ticks may persist — the design does not claim to remove them — but conservation must be exact.

Then read `hoClamp` across every run produced in this task:

```bash
grep -ho "hoClamp=[0-9]*" runs/exp-custody-*/*/mid.log | sort | uniq -c
```

If every value is 0, backlog item 7 closes as unreachable **with evidence**. If any is non-zero, record the count and leave item 7 open with a real justification.

- [ ] **Step 5: Write the results document and commit**

Create `docs/superpowers/results/2026-08-20-B-custody.md` recording: the no-op verification table with actual measured numbers, the E7 conservation before/after, `hoResent` / `hoReclaimed` totals, the E4 result, and the `hoClamp` verdict on item 7. State plainly whether the ownership *gap* still exists — it should, and that is not a failure of this work.

```bash
git add docs/superpowers/results/2026-08-20-B-custody.md
git commit -m "docs(B): custody re-measurement results"
```

---

### Task 7: Close the backlog and correct the docs

**Files:**
- Modify: `docs/EVALUATION.md` (§6 conditional guarantees, §7 backlog items 1, 2, 6, 7)
- Modify: `CLAUDE.md` (the ownership-gap bullet in the verified-state warnings)

**Interfaces:**
- Consumes: Task 6's measured results.
- Produces: nothing.

- [ ] **Step 1: Correct the lookahead-0 overstatement in CLAUDE.md**

Find the bullet in the verified-state warnings beginning **"The handoff ack is still stubbed"**. Replace that bullet with the following (measurement-independent, use verbatim):

```markdown
> - **Handoff transfers are lossless; the ownership gap is not closed.** These are two different
>   claims and this file previously ran them together. The ack path is live: the receiver acks on
>   acceptance, and the sender keeps the transfer packet until that ack arrives, resending on timeout
>   and reclaiming the object if it never lands (`DistributedSystemCommonFiles/HandoffCustody.h`,
>   `ServerWorldManager::FlushPendingTransfers`). An object can no longer vanish because a receiver
>   refused it or died. **The gap itself remains**: at `--handoff-lookahead 0` - the DEFAULT -
>   `ScheduleOutgoingObject` releases on send, so nobody owns the object for one network latency.
>   Only `--handoff-lookahead > 0` releases on the receiver's install tick, and the claim that
>   "ownership changes atomically" was only ever true in that mode. Most experiments ran at 0.
```

If the bullet's exact opening text differs, locate it with `grep -n "handoff ack is still stubbed" CLAUDE.md` and replace the whole bullet.

- [ ] **Step 2: Close items 1 and 2, restate item 6, resolve item 7**

In `docs/EVALUATION.md` §7:
- **Item 1** — struck through, marked fixed, citing the custody mechanism and Task 6's E7 conservation result.
- **Item 2** — struck through and **narrowed rather than claimed fixed**: transfers are now lossless, but the ownership *gap* under drift remains and stays a conditional guarantee. Say which of the two the work actually addressed.
- **Item 6** — replace the whole entry with the following (measurement-independent, use verbatim):

```markdown
6. ~~**Load profile buckets objects, not contacts.**~~ **Withdrawn — this is a deliberate design
   decision, not a defect.** `TakeLoadReport` states the reason in place: a contact belongs to two
   objects that may fall in different buckets, so charging it to either one is arbitrary, while
   object count within a bucket is a sound proxy because contact cost scales with local density.
   What remains true is the *consequence*, and it stays recorded as a limitation in §6: the balancer
   equalises objects, so E4 ends with near-perfect object balance and a residual contact imbalance
   (14.2M vs 8.1M). That is a stated property of object-count balancing, not an unfixed bug.
```
- **Item 7** — closed as unreachable with the `hoClamp` evidence, or left open with the measured count.

Update §6 so the "ownership atomicity holds only while both servers keep pace" paragraph distinguishes *losing* an object (fixed) from *the gap* (unchanged).

- [ ] **Step 3: Verify no stale cross-references remain**

```bash
grep -n "items 1, 2, 6\|Batch B\|must be re-run" docs/EVALUATION.md
grep -rn "never called\|empty body\|stubbed" docs/EVALUATION.md CLAUDE.md | grep -i "handshake\|transition\|CalculateIncoming"
```

Every hit must either be struck through, marked fixed, or still be genuinely true.

- [ ] **Step 4: Commit**

```bash
git add docs/EVALUATION.md CLAUDE.md
git commit -m "docs: close backlog items 1, 2, 6, 7"
```

---

## Notes for the executor

- **Task 3 Step 3 contains a deliberate trap** flagged in its own implementer note: the scratch packet dereferences a null pointer. Add the defaulted constructor described there; do not ship the null dereference.
- **Task 6 Step 2 is a hard gate.** If the no-op verification does not reproduce exactly, stop and report rather than continuing — Batch A produced exactly this situation and the reproduction check is what caught an incorrect no-op claim.
- **`runs/` is gitignored.** Measurement outputs are not committed; the results document is the durable record.
- **Do not push.** The branch is many commits ahead of origin and the user has not asked for a push or a merge.
