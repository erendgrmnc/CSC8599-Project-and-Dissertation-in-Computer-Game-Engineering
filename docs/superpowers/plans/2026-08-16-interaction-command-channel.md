# Interaction Command Channel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a client issue an interaction — push an object, drive an object along an axis — that is executed by whichever server actually owns that object, correctly and exactly once, even when the client's belief about the owner is stale.

**Architecture:** One packet shape carries every interaction; the payload is interpreted by an `IInteractionCommand` looked up in a registry by `CommandType`. Adding a new interaction adds **zero** message types and touches **zero** switch statements. The client resolves a target server from state it already maintains (`mObjectOwner`, `mServerRegions`) and sends reliably to that link. A server that receives a command for an object it does not own **relays** it to the true owner rather than rejecting it, and the applying server acks the client directly. Dedupe is by `(playerID, sequence)` because reliability does not compose across a relay hop.

**Tech Stack:** C++20, MSVC x64, ENet, CMake (Visual Studio 17 2022 generator). Tests use the `tools/InteractionTests` harness.

**Spec:** `docs/superpowers/specs/2026-08-16-interactions-toolset-design.md` — §1 (authority model), §2 (command channel), §6 (API surface), §7.3 (test tiers).

## Global Constraints

- **`BasicNetworkMessages` is APPEND ONLY.** New entries go after the current last entry. The four roles are built and deployed separately (`tools/build-deploy.ps1`) and can be version-mismatched at runtime, so inserting anywhere else silently renumbers the wire protocol. **Do not reclaim the ~15 dead team-game entries.**
- **Every packet is strict POD.** The ENet path `memcpy`s packet structs verbatim. No `std::string`, no `std::vector`, no pointers, no virtuals in any packet. Every new packet gets a `static_assert(std::is_trivially_copyable_v<T>)` and a size assertion next to it.
- **Packet `size` field convention:** `size = sizeof(T) - sizeof(GamePacket)`.
- **Toolset headers must be free of `USEGL` and `DISTRIBUTEDSYSTEMACTIVE` guards** so the Tier 0 test target can include them. This is why they live in `DistributedSystemCommonFiles/`, not in `NetworkObject.h`.
- **C++20**, `NCL` root namespace, `mCamelCase` members, `PascalCase` methods, `SCREAMING_SNAKE` file-scope constants.
- **Add new files to the owning `CMake*.cmake`.**
- **Commit convention:** short lowercase `type(scope): summary`, no co-author trailers, one logical change per commit.

## Prerequisite

Increment 1 (`docs/superpowers/plans/2026-08-16-physics-dynamic-registration.md`) must be complete
— not for its code, which this plan does not use, but for `tools/InteractionTests`, which every
task here tests against.

---

## File Structure

| File | Responsibility |
|---|---|
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h` | *Create.* Enums, `CommandArgs`, `CommandScope`, `IInteractionCommand`, `ICommandContext`, `CommandRegistry`. Guard-free so tests can include it. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.cpp` | *Create.* `CommandRegistry` bodies and `RegisterDefaults`. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/SequenceWindow.h` | *Create.* Duplicate detection under out-of-order arrival. Header-only, pure logic. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommands.cpp` | *Create.* The `MoveAxis` and `Impulse` implementations. |
| `CSC8503CoreClasses/NetworkBase.h` | *Modify.* Three appended enum entries. |
| `CSC8503CoreClasses/NetworkObject.h` | *Modify.* Three packet structs + asserts. |
| `CSC8503CoreClasses/NetworkObject.cpp` | *Modify.* Their constructors. |
| `DistributedGameServer/ServerWorldManager.h` / `.cpp` | *Modify.* Implements `ICommandContext`. |
| `DistributedGameServer/DistributedGameServerManager.h` / `.cpp` | *Modify.* Receive, dedupe, apply, ack, drain the relay queue. |
| `CSC8503/DistributedMultiplayerGameScene.h` / `.cpp` | *Modify.* Sequence counter, `ResolveCommandTarget`, send, ack handling. |
| `tools/InteractionTests/CommandChannelTests.cpp` | *Create.* Tier 0 coverage. |

---

### Task 1: Command types, registry and the sequence window

Pure data and pure logic — no engine, no network. This is the layer everything else is expressed
in, and it is fully unit-testable.

**Files:**
- Create: `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h`
- Create: `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.cpp`
- Create: `CSC8503CoreClasses/DistributedSystemCommonFiles/SequenceWindow.h`
- Create: `tools/InteractionTests/CommandChannelTests.cpp`
- Modify: `CSC8503CoreClasses/CMakePC.cmake`
- Modify: `tools/InteractionTests/CMakeLists.txt`

**Interfaces:**
- Consumes: the `TEST`/`CHECK` harness from increment 1 Task 1.
- Produces: `NCL::Interaction::{CommandType, CommandResult, DespawnReason, CommandArgs, CommandScope, IInteractionCommand, ICommandContext, CommandRegistry}`; `NCL::SequenceWindow`.

- [x] **Step 1: Write the failing tests**

`tools/InteractionTests/CommandChannelTests.cpp`:

```cpp
#include "TestHarness.h"

#include "DistributedSystemCommonFiles/InteractionCommand.h"
#include "DistributedSystemCommonFiles/SequenceWindow.h"

#include <type_traits>

using namespace NCL;
using namespace NCL::Interaction;

// CommandArgs rides inside a memcpy'd packet. If anyone adds a std::string to it,
// this must break the build rather than corrupt the wire across machines.
TEST(CommandArgsIsTriviallyCopyable) {
	CHECK(std::is_trivially_copyable_v<CommandArgs>);
	CHECK(std::is_trivially_copyable_v<CommandScope>);
}

TEST(RegistryReturnsNullForUnregisteredType) {
	CommandRegistry registry;
	CHECK(registry.Find(CommandType::Teleport) == nullptr);
}

namespace {
	// Minimal stand-in so the registry can be tested without any real command.
	class StubCommand : public IInteractionCommand {
	public:
		explicit StubCommand(CommandType type) : mType(type) {}
		CommandType GetType() const override { return mType; }
		CommandScope GetScope(const CommandArgs&) const override { return CommandScope{}; }
		CommandResult Apply(ICommandContext&, const CommandArgs&) override {
			return CommandResult::Applied;
		}
	private:
		CommandType mType;
	};
}

TEST(RegistryFindsRegisteredCommand) {
	CommandRegistry registry;
	registry.Register(std::make_unique<StubCommand>(CommandType::Impulse));

	IInteractionCommand* found = registry.Find(CommandType::Impulse);
	CHECK(found != nullptr);
	if (found != nullptr) {
		CHECK(found->GetType() == CommandType::Impulse);
	}
	CHECK(registry.Find(CommandType::MoveAxis) == nullptr);
}

// Registering the same type twice must not leave two handlers - the second wins
// and the first is released, so a command can never be applied twice.
TEST(RegistryReplacesOnDuplicateType) {
	CommandRegistry registry;
	registry.Register(std::make_unique<StubCommand>(CommandType::Impulse));
	registry.Register(std::make_unique<StubCommand>(CommandType::Impulse));
	CHECK(registry.Find(CommandType::Impulse) != nullptr);
}

TEST(SequenceWindowAcceptsEachSequenceOnce) {
	SequenceWindow window;
	CHECK(window.Accept(1));
	CHECK(!window.Accept(1));
	CHECK(window.Accept(2));
	CHECK(!window.Accept(2));
}

// A relayed command and a directly routed one have no ordering relationship, so
// sequences genuinely arrive out of order. Older-but-unseen must still be accepted.
TEST(SequenceWindowAcceptsOutOfOrderWithinWindow) {
	SequenceWindow window;
	CHECK(window.Accept(10));
	CHECK(window.Accept(7));
	CHECK(!window.Accept(7));
	CHECK(window.Accept(11));
}

// Beyond the window there is no memory, so anything that old is refused rather
// than risking a double-apply.
TEST(SequenceWindowRejectsBeyondWindow) {
	SequenceWindow window;
	for (int sequence = 1; sequence <= 200; ++sequence) {
		CHECK(window.Accept(sequence));
	}
	CHECK(!window.Accept(1));
	CHECK(!window.Accept(100));
}
```

- [x] **Step 2: Run — must fail to compile**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
```

Expected: `fatal error C1083: Cannot open include file: 'DistributedSystemCommonFiles/InteractionCommand.h'`.

- [x] **Step 3: Write `SequenceWindow.h`**

```cpp
#pragma once
#include <array>

namespace NCL {

	// Duplicate detection for client commands.
	//
	// Client -> server is reliable, so ENet already guarantees exactly-once on THAT
	// link. Reliability does not compose across a relay hop: a relayed command is a
	// separate reliable send on a different link with no ordering relationship to the
	// first, so the same command can arrive twice and out of order. Hence a high-water
	// mark plus a small ring of recently seen sequences, not just a counter.
	class SequenceWindow {
	public:
		static constexpr int WINDOW_SIZE = 64;

		// True if this sequence has not been seen before and is recent enough to
		// judge. False means "duplicate, or too old to be sure" - both are dropped.
		bool Accept(int sequence) {
			if (sequence <= mHighWater - WINDOW_SIZE) {
				return false;   // Older than anything we still remember.
			}

			if (sequence > mHighWater) {
				mHighWater = sequence;
				mSeen[Slot(sequence)] = sequence;
				return true;
			}

			if (mSeen[Slot(sequence)] == sequence) {
				return false;   // Already applied.
			}

			mSeen[Slot(sequence)] = sequence;
			return true;
		}

		int GetHighWater() const {
			return mHighWater;
		}

	private:
		static int Slot(int sequence) {
			return ((sequence % WINDOW_SIZE) + WINDOW_SIZE) % WINDOW_SIZE;
		}

		// Sentinel below every legal sequence; sequences start at 1.
		int mHighWater = 0;
		std::array<int, WINDOW_SIZE> mSeen{};
	};
}
```

- [x] **Step 4: Write `InteractionCommand.h`**

Copy the full declaration from spec §6.2 verbatim. It defines `CommandType`, `CommandResult`,
`DespawnReason`, `CommandArgs`, `CommandScope`, `IInteractionCommand`, `ICommandContext` and
`CommandRegistry`. Two constraints when transcribing:

- **No `USEGL` or `DISTRIBUTEDSYSTEMACTIVE` guards anywhere in the file.** The test target and the
  client both include it, and the client build does not define `DISTRIBUTEDSYSTEMACTIVE`.
- **`CommandArgs` stays POD.** Only `int`, `float` and `Maths::Vector3` members.

Add above `CommandType`:

```cpp
	// Wire values, APPEND ONLY: these travel inside DistributedClientCommandPacket.
	// The three server roles and the client are built and deployed separately, so a
	// renumber here is a silent cross-version misinterpretation, not a build error.
```

- [x] **Step 5: Write `InteractionCommand.cpp`**

```cpp
#include "InteractionCommand.h"

namespace NCL::Interaction {

	CommandRegistry& CommandRegistry::Instance() {
		static CommandRegistry instance;
		return instance;
	}

	void CommandRegistry::Register(std::unique_ptr<IInteractionCommand> command) {
		if (command == nullptr) {
			return;
		}
		// operator[] + move rather than insert: a second registration for a type must
		// REPLACE, never sit alongside, or the command could be applied twice.
		const CommandType type = command->GetType();
		mCommands[type] = std::move(command);
	}

	IInteractionCommand* CommandRegistry::Find(CommandType type) const {
		const auto entry = mCommands.find(type);
		if (entry == mCommands.end()) {
			return nullptr;
		}
		return entry->second.get();
	}
}
```

`RegisterDefaults` is added in Task 3, when there are defaults to register.

- [x] **Step 6: Register the sources in CMake**

In `CSC8503CoreClasses/CMakePC.cmake`, alongside the other `DistributedSystemCommonFiles` entries,
add `DistributedSystemCommonFiles/InteractionCommand.h`, `InteractionCommand.cpp` and
`SequenceWindow.h`.

In `tools/InteractionTests/CMakeLists.txt`, add `"CommandChannelTests.cpp"` to the
`add_executable` source list.

- [x] **Step 7: Run the tests — all must pass**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: the 7 new tests pass alongside increment 1's 10.

- [x] **Step 8: Commit**

```bash
git add CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.cpp CSC8503CoreClasses/DistributedSystemCommonFiles/SequenceWindow.h CSC8503CoreClasses/CMakePC.cmake tools/InteractionTests
git commit -m "feat(interaction): command types, registry and sequence window"
```

---

### Task 2: Wire protocol — enum entries and packets

**Files:**
- Modify: `CSC8503CoreClasses/NetworkBase.h` (append to `BasicNetworkMessages`, after `DistributedClientSnapshotAck`)
- Modify: `CSC8503CoreClasses/NetworkObject.h` (packet structs, inside the existing `#ifdef USEGL` block)
- Modify: `CSC8503CoreClasses/NetworkObject.cpp` (constructors)
- Test: `tools/InteractionTests/CommandChannelTests.cpp`

**Interfaces:**
- Consumes: `NCL::Interaction::CommandArgs` from Task 1.
- Produces: `DistributedClientCommandPacket`, `DistributedCommandAckPacket`, `DistributedServerCommandRelayPacket`; enum entries `DistributedClientCommand`, `DistributedCommandAck`, `DistributedServerCommandRelay`.

> Only three of the spec's five message types are added here. `DistributedObjectSpawned` and
> `DistributedObjectDespawned` belong to increments 5 and 6; adding them now would put unused
> entries on the wire before their semantics are settled.

- [x] **Step 1: Write the failing tests**

Append to `CommandChannelTests.cpp`:

```cpp
#include "NetworkObject.h"

// The size field convention is size = sizeof(T) - sizeof(GamePacket). Getting this
// wrong truncates the payload on the wire and only shows up across machines.
TEST(ClientCommandPacketHasCorrectLayout) {
	CHECK(std::is_trivially_copyable_v<NCL::CSC8503::DistributedClientCommandPacket>);

	CommandArgs args;
	args.targetObjectID = 7;
	args.magnitude = 12.5f;

	NCL::CSC8503::DistributedClientCommandPacket packet(
		static_cast<int>(CommandType::Impulse), 3, 1, args);

	CHECK_EQ(packet.type, (short)NCL::BasicNetworkMessages::DistributedClientCommand);
	CHECK_EQ((size_t)packet.size + sizeof(NCL::CSC8503::GamePacket),
		sizeof(NCL::CSC8503::DistributedClientCommandPacket));
	CHECK_EQ(packet.commandType, (int)CommandType::Impulse);
	CHECK_EQ(packet.sequence, 3);
	CHECK_EQ(packet.hintServerID, 1);
	CHECK_EQ(packet.args.targetObjectID, 7);
	CHECK_NEAR(packet.args.magnitude, 12.5f, 1e-6);
}

TEST(CommandAckPacketHasCorrectLayout) {
	CHECK(std::is_trivially_copyable_v<NCL::CSC8503::DistributedCommandAckPacket>);

	NCL::CSC8503::DistributedCommandAckPacket packet(
		9, 2, 42, static_cast<int>(CommandResult::NotOwner), 1);

	CHECK_EQ(packet.type, (short)NCL::BasicNetworkMessages::DistributedCommandAck);
	CHECK_EQ((size_t)packet.size + sizeof(NCL::CSC8503::GamePacket),
		sizeof(NCL::CSC8503::DistributedCommandAckPacket));
	CHECK_EQ(packet.sequence, 9);
	CHECK_EQ(packet.playerID, 2);
	CHECK_EQ(packet.targetObjectID, 42);
	CHECK_EQ(packet.result, (int)CommandResult::NotOwner);
	CHECK_EQ(packet.correctedServerID, 1);
}

TEST(ServerCommandRelayPacketHasCorrectLayout) {
	CHECK(std::is_trivially_copyable_v<NCL::CSC8503::DistributedServerCommandRelayPacket>);

	CommandArgs args;
	args.targetObjectID = 42;

	NCL::CSC8503::DistributedServerCommandRelayPacket packet(
		static_cast<int>(CommandType::Impulse), 0, 5, 2, 9, args);

	CHECK_EQ(packet.type, (short)NCL::BasicNetworkMessages::DistributedServerCommandRelay);
	CHECK_EQ((size_t)packet.size + sizeof(NCL::CSC8503::GamePacket),
		sizeof(NCL::CSC8503::DistributedServerCommandRelayPacket));
	CHECK_EQ(packet.originServerID, 0);
	CHECK_EQ(packet.originSequence, 5);
	CHECK_EQ(packet.playerID, 2);
	CHECK_EQ(packet.clientSequence, 9);
	// A relay must never be re-relayed; receiving one with hopCount > 0 is a bug.
	CHECK_EQ(packet.hopCount, 0);
	CHECK_EQ(packet.args.targetObjectID, 42);
}
```

- [x] **Step 2: Run — must fail to compile**

Expected: `error C2065: 'DistributedClientCommand': undeclared identifier`.

- [x] **Step 3: Append the enum entries**

In `CSC8503CoreClasses/NetworkBase.h`, after `DistributedClientSnapshotAck` (the current last
entry) — note it currently has no trailing comma:

```cpp
	DistributedClientSnapshotAck,
	//Dynamic Interaction Packet Types. APPEND ONLY - see the comment above.
	DistributedClientCommand,        // Client      -> Game Server
	DistributedCommandAck,           // Game Server -> Client
	DistributedServerCommandRelay    // Game Server -> Game Server
};
```

- [x] **Step 4: Add the packet structs**

In `CSC8503CoreClasses/NetworkObject.h`, inside the existing `#ifdef USEGL` block with the other
packets. Include `DistributedSystemCommonFiles/InteractionCommand.h` at the top of the file.

```cpp
	// Client -> owning game server. Reliable. One packet shape for every interaction;
	// the payload is interpreted by the IInteractionCommand registered for commandType.
	struct DistributedClientCommandPacket : public GamePacket {
		int commandType;                        // NCL::Interaction::CommandType
		int sequence;                           // per-client monotonic; dedupe + ack key
		int hintServerID;                       // client's belief about the owner, -1 = unknown
		NCL::Interaction::CommandArgs args;

		DistributedClientCommandPacket(int commandType, int sequence, int hintServerID,
			const NCL::Interaction::CommandArgs& args);
	};
	static_assert(std::is_trivially_copyable_v<DistributedClientCommandPacket>);

	// Game server -> the issuing client. Reliable. Sent by whichever server APPLIED
	// the command, which is not necessarily the one that received it.
	struct DistributedCommandAckPacket : public GamePacket {
		int sequence;
		int playerID;
		int targetObjectID;                     // so the client can correct mObjectOwner
		int result;                             // NCL::Interaction::CommandResult
		int correctedServerID;                  // on NotOwner: the true owner; else -1

		DistributedCommandAckPacket(int sequence, int playerID, int targetObjectID, int result,
			int correctedServerID);
	};
	static_assert(std::is_trivially_copyable_v<DistributedCommandAckPacket>);

	// Game server -> game server. Carries the client's identity so the true owner can
	// ack the client directly, and the relaying server's identity for the dedupe key.
	struct DistributedServerCommandRelayPacket : public GamePacket {
		int commandType;
		int originServerID;
		int originSequence;                     // (originServerID, originSequence) = dedupe key
		int hopCount;                           // 0 on send; >0 on receive is a bug: drop + count
		int playerID;
		int clientSequence;                     // so the applying server can ack the client
		NCL::Interaction::CommandArgs args;

		DistributedServerCommandRelayPacket(int commandType, int originServerID, int originSequence,
			int playerID, int clientSequence, const NCL::Interaction::CommandArgs& args);
	};
	static_assert(std::is_trivially_copyable_v<DistributedServerCommandRelayPacket>);
```

Add `#include <type_traits>` if `std::is_trivially_copyable_v` does not resolve.

- [x] **Step 5: Add the constructors**

In `CSC8503CoreClasses/NetworkObject.cpp`:

```cpp
DistributedClientCommandPacket::DistributedClientCommandPacket(int commandType, int sequence,
	int hintServerID, const NCL::Interaction::CommandArgs& args) {
	type = BasicNetworkMessages::DistributedClientCommand;
	size = sizeof(DistributedClientCommandPacket) - sizeof(GamePacket);

	this->commandType = commandType;
	this->sequence = sequence;
	this->hintServerID = hintServerID;
	this->args = args;
}

DistributedCommandAckPacket::DistributedCommandAckPacket(int sequence, int playerID,
	int targetObjectID, int result, int correctedServerID) {
	type = BasicNetworkMessages::DistributedCommandAck;
	size = sizeof(DistributedCommandAckPacket) - sizeof(GamePacket);

	this->sequence = sequence;
	this->playerID = playerID;
	this->targetObjectID = targetObjectID;
	this->result = result;
	this->correctedServerID = correctedServerID;
}

DistributedServerCommandRelayPacket::DistributedServerCommandRelayPacket(int commandType,
	int originServerID, int originSequence, int playerID, int clientSequence,
	const NCL::Interaction::CommandArgs& args) {
	type = BasicNetworkMessages::DistributedServerCommandRelay;
	size = sizeof(DistributedServerCommandRelayPacket) - sizeof(GamePacket);

	this->commandType = commandType;
	this->originServerID = originServerID;
	this->originSequence = originSequence;
	// Always 0 on send. A relay is never re-relayed; see the hop check in Task 5.
	this->hopCount = 0;
	this->playerID = playerID;
	this->clientSequence = clientSequence;
	this->args = args;
}
```

- [x] **Step 6: Run the tests — all must pass**

Expected: 3 new layout tests pass.

If `CommandChannelTests.cpp` fails to compile on `NetworkObject.h` because of `USEGL`, add
`USEGL` to the test target's `target_compile_definitions` — the root `CMakeLists.txt` sets it
globally for x64, so this should already hold.

- [x] **Step 7: Commit**

```bash
git add CSC8503CoreClasses/NetworkBase.h CSC8503CoreClasses/NetworkObject.h CSC8503CoreClasses/NetworkObject.cpp tools/InteractionTests/CommandChannelTests.cpp
git commit -m "feat(net): interaction command, ack and relay packets"
```

---

### Task 3: The `MoveAxis` and `Impulse` commands

Two concrete commands, testable against a fake `ICommandContext` with no engine and no network.
This is where the "adding an interaction touches no switch statement" claim is proven.

**Files:**
- Create: `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommands.cpp`
- Modify: `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.cpp` (`RegisterDefaults`)
- Modify: `CSC8503CoreClasses/CMakePC.cmake`
- Test: `tools/InteractionTests/CommandChannelTests.cpp`

**Interfaces:**
- Consumes: `IInteractionCommand`, `ICommandContext`, `CommandRegistry` from Task 1.
- Produces: `CommandRegistry::RegisterDefaults()` registering `CommandType::MoveAxis` and `CommandType::Impulse`.

- [x] **Step 1: Write the failing tests**

Append to `CommandChannelTests.cpp`:

```cpp
#include <vector>

namespace {
	// Records what a command asked the world to do, so command logic can be tested
	// with no GameWorld, no PhysicsSystem and no sockets.
	class FakeContext : public ICommandContext {
	public:
		int serverID = 0;
		int owningServerResult = 0;
		bool objectIsActiveHere = true;
		Maths::Vector3 lastKnownPosition{ 0, 0, 0 };
		bool hasLastKnownPosition = true;

		struct ImpulseCall { int objectID; Maths::Vector3 impulse; };
		struct MoveAxisCall { int objectID; int playerID; Maths::Vector3 axis; };
		struct RelayCall { int serverID; CommandType type; };

		std::vector<ImpulseCall> impulses;
		std::vector<MoveAxisCall> moveAxes;
		std::vector<RelayCall> relays;

		int GetServerID() const override { return serverID; }
		int GetOwningServer(const Maths::Vector3&) const override { return owningServerResult; }

		CSC8503::GameObject* FindActiveObject(int) const override {
			// Non-null only matters as a yes/no here; commands must not dereference it.
			return objectIsActiveHere ? reinterpret_cast<CSC8503::GameObject*>(1) : nullptr;
		}

		bool TryGetLastKnownPosition(int, Maths::Vector3& out) const override {
			if (!hasLastKnownPosition) {
				return false;
			}
			out = lastKnownPosition;
			return true;
		}

		int SpawnObject(int, const Maths::Vector3&, int) override { return -1; }
		bool DestroyObject(int, DespawnReason, int) override { return false; }

		void ApplyImpulse(int objectID, const Maths::Vector3& impulse) override {
			impulses.push_back({ objectID, impulse });
		}
		void ApplyRadialImpulse(const Maths::Vector3&, float, float) override {}
		void SetMoveAxis(int objectID, int playerID, const Maths::Vector3& axis) override {
			moveAxes.push_back({ objectID, playerID, axis });
		}
		void RelayToServer(int serverID, CommandType type, const CommandArgs&) override {
			relays.push_back({ serverID, type });
		}
		void GetOverlappedServers(const Maths::Vector3&, float, std::vector<int>&) const override {}
	};
}

TEST(RegisterDefaultsProvidesMoveAxisAndImpulse) {
	CommandRegistry registry;
	registry.RegisterDefaultsInto(registry);
	CHECK(registry.Find(CommandType::MoveAxis) != nullptr);
	CHECK(registry.Find(CommandType::Impulse) != nullptr);
}

TEST(ImpulseAppliesToOwnedObject) {
	CommandRegistry registry;
	registry.RegisterDefaultsInto(registry);
	IInteractionCommand* impulse = registry.Find(CommandType::Impulse);

	FakeContext ctx;
	ctx.objectIsActiveHere = true;

	CommandArgs args;
	args.targetObjectID = 5;
	args.direction = Maths::Vector3(1, 0, 0);
	args.magnitude = 10.0f;

	const CommandResult result = impulse->Apply(ctx, args);

	CHECK(result == CommandResult::Applied);
	CHECK_EQ((int)ctx.impulses.size(), 1);
	if (!ctx.impulses.empty()) {
		CHECK_EQ(ctx.impulses[0].objectID, 5);
		CHECK_NEAR(ctx.impulses[0].impulse.x, 10.0f, 1e-5);
	}
}

// The core of the authority model: a server that does not own the object forwards
// rather than rejecting, so a stale client owner-table still results in the push
// happening.
TEST(ImpulseRelaysWhenNotOwner) {
	CommandRegistry registry;
	registry.RegisterDefaultsInto(registry);
	IInteractionCommand* impulse = registry.Find(CommandType::Impulse);

	FakeContext ctx;
	ctx.serverID = 0;
	ctx.objectIsActiveHere = false;          // not ours
	ctx.hasLastKnownPosition = true;
	ctx.lastKnownPosition = Maths::Vector3(80, 0, 0);
	ctx.owningServerResult = 1;              // ...it belongs to server 1

	CommandArgs args;
	args.targetObjectID = 5;
	args.direction = Maths::Vector3(1, 0, 0);
	args.magnitude = 10.0f;

	const CommandResult result = impulse->Apply(ctx, args);

	CHECK(result == CommandResult::Relayed);
	CHECK_EQ((int)ctx.impulses.size(), 0);
	CHECK_EQ((int)ctx.relays.size(), 1);
	if (!ctx.relays.empty()) {
		CHECK_EQ(ctx.relays[0].serverID, 1);
	}
}

// No last-known position means nowhere to forward to. Reporting ObjectUnknown is
// what lets the I4 accounting invariant balance.
TEST(ImpulseReportsUnknownWhenNoPositionKnown) {
	CommandRegistry registry;
	registry.RegisterDefaultsInto(registry);
	IInteractionCommand* impulse = registry.Find(CommandType::Impulse);

	FakeContext ctx;
	ctx.objectIsActiveHere = false;
	ctx.hasLastKnownPosition = false;

	CommandArgs args;
	args.targetObjectID = 5;
	args.direction = Maths::Vector3(1, 0, 0);
	args.magnitude = 10.0f;

	CHECK(impulse->Apply(ctx, args) == CommandResult::ObjectUnknown);
	CHECK_EQ((int)ctx.relays.size(), 0);
}

// A zero direction would be a no-op impulse; rejecting it keeps the accounting
// honest rather than counting it as applied.
TEST(ImpulseRejectsZeroDirection) {
	CommandRegistry registry;
	registry.RegisterDefaultsInto(registry);
	IInteractionCommand* impulse = registry.Find(CommandType::Impulse);

	CommandArgs args;
	args.targetObjectID = 5;
	args.direction = Maths::Vector3(0, 0, 0);
	args.magnitude = 10.0f;

	CHECK(!impulse->Validate(args));
}

TEST(ImpulseScopeTargetsObject) {
	CommandRegistry registry;
	registry.RegisterDefaultsInto(registry);

	CommandArgs args;
	args.targetObjectID = 5;
	const CommandScope scope = registry.Find(CommandType::Impulse)->GetScope(args);

	CHECK(scope.targetsObject);
	CHECK(!scope.isContinuous);
}

// MoveAxis is continuous state, not an event: it must never be sequenced or relayed.
TEST(MoveAxisScopeIsContinuous) {
	CommandRegistry registry;
	registry.RegisterDefaultsInto(registry);

	CommandArgs args;
	args.targetObjectID = 5;
	const CommandScope scope = registry.Find(CommandType::MoveAxis)->GetScope(args);

	CHECK(scope.isContinuous);
	CHECK(scope.targetsObject);
}

TEST(MoveAxisAppliesToOwnedObject) {
	CommandRegistry registry;
	registry.RegisterDefaultsInto(registry);

	FakeContext ctx;
	ctx.objectIsActiveHere = true;

	CommandArgs args;
	args.targetObjectID = 5;
	args.playerID = 2;
	args.direction = Maths::Vector3(0, 0, 1);

	CHECK(registry.Find(CommandType::MoveAxis)->Apply(ctx, args) == CommandResult::Applied);
	CHECK_EQ((int)ctx.moveAxes.size(), 1);
	if (!ctx.moveAxes.empty()) {
		CHECK_EQ(ctx.moveAxes[0].playerID, 2);
	}
}

// Continuous state is dropped, never forwarded: a relayed axis would arrive stale
// and fight the owner's own input stream.
TEST(MoveAxisDoesNotRelay) {
	CommandRegistry registry;
	registry.RegisterDefaultsInto(registry);

	FakeContext ctx;
	ctx.objectIsActiveHere = false;
	ctx.hasLastKnownPosition = true;
	ctx.owningServerResult = 1;

	CommandArgs args;
	args.targetObjectID = 5;
	args.playerID = 2;
	args.direction = Maths::Vector3(0, 0, 1);

	CHECK(registry.Find(CommandType::MoveAxis)->Apply(ctx, args) == CommandResult::NotOwner);
	CHECK_EQ((int)ctx.relays.size(), 0);
	CHECK_EQ((int)ctx.moveAxes.size(), 0);
}
```

- [x] **Step 2: Run — must fail to compile**

Expected: `error C2039: 'RegisterDefaultsInto': is not a member of 'CommandRegistry'`.

- [x] **Step 3: Add `RegisterDefaultsInto` to the registry**

The spec's `RegisterDefaults()` is a static that populates the singleton. Tests need to populate a
local instance, so add both. In `InteractionCommand.h`, in `CommandRegistry`'s public section:

```cpp
		// Registers every built-in interaction. RegisterDefaults() fills the process
		// singleton; RegisterDefaultsInto() exists so tests can use a local registry
		// and stay independent of one another.
		static void RegisterDefaults();
		static void RegisterDefaultsInto(CommandRegistry& registry);
```

- [x] **Step 4: Implement the commands**

`CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommands.cpp`:

```cpp
#include "InteractionCommand.h"

#include <cmath>

namespace NCL::Interaction {

	namespace {
		// Below this, a direction vector has no usable heading.
		constexpr float MIN_DIRECTION_LENGTH = 1e-4f;

		Maths::Vector3 NormalisedOrZero(const Maths::Vector3& v) {
			const float lengthSquared = v.x * v.x + v.y * v.y + v.z * v.z;
			if (lengthSquared < MIN_DIRECTION_LENGTH * MIN_DIRECTION_LENGTH) {
				return Maths::Vector3(0, 0, 0);
			}
			const float length = std::sqrt(lengthSquared);
			return Maths::Vector3(v.x / length, v.y / length, v.z / length);
		}

		// A one-shot push on a single object. Object-targeted, so the owner is
		// whichever server currently has it active.
		class ImpulseCommand : public IInteractionCommand {
		public:
			CommandType GetType() const override { return CommandType::Impulse; }

			CommandScope GetScope(const CommandArgs&) const override {
				CommandScope scope;
				scope.targetsObject = true;
				return scope;
			}

			bool Validate(const CommandArgs& args) const override {
				if (args.targetObjectID < 0) {
					return false;
				}
				const Maths::Vector3 direction = NormalisedOrZero(args.direction);
				if (direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f) {
					return false;
				}
				return args.magnitude > 0.0f;
			}

			CommandResult Apply(ICommandContext& ctx, const CommandArgs& args) override {
				if (!Validate(args)) {
					return CommandResult::Rejected;
				}

				if (ctx.FindActiveObject(args.targetObjectID) != nullptr) {
					const Maths::Vector3 direction = NormalisedOrZero(args.direction);
					ctx.ApplyImpulse(args.targetObjectID, Maths::Vector3(
						direction.x * args.magnitude,
						direction.y * args.magnitude,
						direction.z * args.magnitude));
					return CommandResult::Applied;
				}

				// Not ours. Forward to whoever the object was last seen with rather
				// than rejecting - the client's owner table is allowed to be stale.
				Maths::Vector3 lastKnown;
				if (!ctx.TryGetLastKnownPosition(args.targetObjectID, lastKnown)) {
					return CommandResult::ObjectUnknown;
				}

				const int owner = ctx.GetOwningServer(lastKnown);
				if (owner < 0 || owner == ctx.GetServerID()) {
					// Either outside the world, or it should have been ours and is not
					// active - nothing useful to forward to.
					return CommandResult::ObjectUnknown;
				}

				ctx.RelayToServer(owner, GetType(), args);
				return CommandResult::Relayed;
			}
		};

		// Continuous movement input. State, not an event: applied every tick until
		// superseded, so it is never sequenced and never relayed (a relayed axis
		// arrives stale and fights the owner's own input stream).
		class MoveAxisCommand : public IInteractionCommand {
		public:
			CommandType GetType() const override { return CommandType::MoveAxis; }

			CommandScope GetScope(const CommandArgs&) const override {
				CommandScope scope;
				scope.targetsObject = true;
				scope.isContinuous = true;
				return scope;
			}

			bool Validate(const CommandArgs& args) const override {
				return args.targetObjectID >= 0 && args.playerID >= 0;
			}

			CommandResult Apply(ICommandContext& ctx, const CommandArgs& args) override {
				if (!Validate(args)) {
					return CommandResult::Rejected;
				}
				if (ctx.FindActiveObject(args.targetObjectID) == nullptr) {
					// Dropped, not forwarded. The client re-sends continuously, so the
					// new owner picks it up within a tick or two on its own.
					return CommandResult::NotOwner;
				}
				ctx.SetMoveAxis(args.targetObjectID, args.playerID, NormalisedOrZero(args.direction));
				return CommandResult::Applied;
			}
		};
	}

	void CommandRegistry::RegisterDefaultsInto(CommandRegistry& registry) {
		registry.Register(std::make_unique<ImpulseCommand>());
		registry.Register(std::make_unique<MoveAxisCommand>());
	}

	void CommandRegistry::RegisterDefaults() {
		RegisterDefaultsInto(Instance());
	}
}
```

- [x] **Step 5: Register the source and run the tests**

Add `DistributedSystemCommonFiles/InteractionCommands.cpp` to `CSC8503CoreClasses/CMakePC.cmake`,
then:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: all 10 new tests pass.

- [x] **Step 6: Commit**

```bash
git add CSC8503CoreClasses/DistributedSystemCommonFiles/ CSC8503CoreClasses/CMakePC.cmake tools/InteractionTests/CommandChannelTests.cpp
git commit -m "feat(interaction): move-axis and impulse commands"
```

---

### Task 4: `ServerWorldManager` implements `ICommandContext`

The bridge from command logic to the world. No command ever sees the network layer, and the
network layer never sees the world.

**Files:**
- Modify: `DistributedGameServer/ServerWorldManager.h` (inherit `NCL::Interaction::ICommandContext`)
- Modify: `DistributedGameServer/ServerWorldManager.cpp`

**Interfaces:**
- Consumes: `ICommandContext` from Task 1.
- Produces: `ServerWorldManager` as an `ICommandContext`; a drainable relay queue
  `bool PopPendingRelay(PendingRelay& out)`.

- [x] **Step 1: Declare the interface**

In `ServerWorldManager.h`, include `DistributedSystemCommonFiles/InteractionCommand.h` and change
the class declaration:

```cpp
		class ServerWorldManager : public NCL::Interaction::ICommandContext {
```

Add to the public section:

```cpp
			// --- ICommandContext ---
			int GetServerID() const override;
			int GetOwningServer(const Maths::Vector3& worldPoint) const override;
			CSC8503::GameObject* FindActiveObject(int networkObjectID) const override;
			bool TryGetLastKnownPosition(int networkObjectID, Maths::Vector3& out) const override;
			int SpawnObject(int archetypeID, const Maths::Vector3& at, int spawnerPlayerID) override;
			bool DestroyObject(int networkObjectID, NCL::Interaction::DespawnReason reason,
				int destroyerPlayerID) override;
			void ApplyImpulse(int networkObjectID, const Maths::Vector3& impulse) override;
			void ApplyRadialImpulse(const Maths::Vector3& origin, float radius, float magnitude) override;
			void SetMoveAxis(int networkObjectID, int playerID, const Maths::Vector3& axis) override;
			void RelayToServer(int serverID, NCL::Interaction::CommandType type,
				const NCL::Interaction::CommandArgs& args) override;
			void GetOverlappedServers(const Maths::Vector3& origin, float radius,
				std::vector<int>& outServerIDs) const override;

			// Relays are queued rather than sent, so a command never re-enters the
			// network layer from inside a packet handler. The manager drains this
			// after Apply returns.
			struct PendingRelay {
				int targetServerID = -1;
				NCL::Interaction::CommandType type = NCL::Interaction::CommandType::None;
				NCL::Interaction::CommandArgs args;
			};
			bool PopPendingRelay(PendingRelay& out);
```

And to the protected data:

```cpp
			std::vector<PendingRelay> mPendingRelays;
```

- [x] **Step 2: Implement the accessors**

In `ServerWorldManager.cpp`:

```cpp
int DistributedGameServer::ServerWorldManager::GetServerID() const {
	return mServerID;
}

int DistributedGameServer::ServerWorldManager::GetOwningServer(const Maths::Vector3& worldPoint) const {
	// Deliberately delegates rather than re-deriving: GetObjectServer is the single
	// place the region test lives, so commands and handoff cannot disagree.
	return GetObjectServer(worldPoint);
}

NCL::CSC8503::GameObject* DistributedGameServer::ServerWorldManager::FindActiveObject(int networkObjectID) const {
	const auto entry = mCreatedObjectPool.find(networkObjectID);
	if (entry == mCreatedObjectPool.end() || entry->second == nullptr) {
		return nullptr;
	}
	// Every server holds a pool entry for every object; only the owner has it active.
	// That is exactly the ownership test a command needs.
	if (!entry->second->IsNetworkActive()) {
		return nullptr;
	}
	return entry->second;
}

bool DistributedGameServer::ServerWorldManager::TryGetLastKnownPosition(int networkObjectID,
	Maths::Vector3& out) const {
	const auto entry = mCreatedObjectPool.find(networkObjectID);
	if (entry == mCreatedObjectPool.end() || entry->second == nullptr) {
		return false;
	}
	out = entry->second->GetTransform().GetPosition();
	return true;
}
```

- [x] **Step 3: Implement the effects**

```cpp
void DistributedGameServer::ServerWorldManager::ApplyImpulse(int networkObjectID,
	const Maths::Vector3& impulse) {
	CSC8503::GameObject* object = FindActiveObject(networkObjectID);
	if (object == nullptr || object->GetPhysicsObject() == nullptr) {
		return;
	}
	object->GetPhysicsObject()->ApplyLinearImpulse(impulse);
}

void DistributedGameServer::ServerWorldManager::ApplyRadialImpulse(const Maths::Vector3& origin,
	float radius, float magnitude) {
	if (radius <= 0.0f) {
		return;
	}
	const float radiusSquared = radius * radius;

	for (auto& entry : mCreatedObjectPool) {
		CSC8503::GameObject* object = entry.second;
		if (object == nullptr || !object->IsNetworkActive() || object->GetPhysicsObject() == nullptr) {
			continue;
		}

		const Maths::Vector3 offset = object->GetTransform().GetPosition() - origin;
		const float distanceSquared = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
		if (distanceSquared > radiusSquared || distanceSquared <= 0.0f) {
			continue;
		}

		// Linear falloff to zero at the radius, so an object exactly on the edge
		// gets nothing and the effect has no discontinuity at the boundary.
		const float distance = std::sqrt(distanceSquared);
		const float falloff = 1.0f - (distance / radius);
		const float scale = (magnitude * falloff) / distance;
		object->GetPhysicsObject()->ApplyLinearImpulse(
			Maths::Vector3(offset.x * scale, offset.y * scale, offset.z * scale));
	}
}

void DistributedGameServer::ServerWorldManager::SetMoveAxis(int networkObjectID, int playerID,
	const Maths::Vector3& axis) {
	CSC8503::GameObject* object = FindActiveObject(networkObjectID);
	if (object == nullptr || object->GetPhysicsObject() == nullptr) {
		return;
	}
	// Applied as a force so it composes with gravity and collisions rather than
	// overwriting the velocity the integrator just produced.
	object->GetPhysicsObject()->AddForce(Maths::Vector3(
		axis.x * MOVE_AXIS_FORCE, axis.y * MOVE_AXIS_FORCE, axis.z * MOVE_AXIS_FORCE));
}
```

Add to the anonymous namespace at the top of `ServerWorldManager.cpp`:

```cpp
	// Force applied per unit of movement-axis input. Tuned against the shuttle
	// workload's 30-60 u/s so a driven object is comparable to a launched one.
	constexpr float MOVE_AXIS_FORCE = 200.0f;
```

`SpawnObject` and `DestroyObject` belong to increments 5 and 6. Stub them honestly:

```cpp
int DistributedGameServer::ServerWorldManager::SpawnObject(int, const Maths::Vector3&, int) {
	// Runtime spawn is increment 5. Returning -1 makes any premature caller fail
	// loudly through the normal ID-exhaustion path rather than half-working.
	return -1;
}

bool DistributedGameServer::ServerWorldManager::DestroyObject(int, NCL::Interaction::DespawnReason, int) {
	return false;   // Runtime destroy is increment 6.
}
```

- [x] **Step 4: Implement relay queueing and region overlap**

```cpp
void DistributedGameServer::ServerWorldManager::RelayToServer(int serverID,
	NCL::Interaction::CommandType type, const NCL::Interaction::CommandArgs& args) {
	if (serverID < 0 || serverID == mServerID) {
		return;
	}
	PendingRelay relay;
	relay.targetServerID = serverID;
	relay.type = type;
	relay.args = args;
	mPendingRelays.push_back(relay);
}

bool DistributedGameServer::ServerWorldManager::PopPendingRelay(PendingRelay& out) {
	if (mPendingRelays.empty()) {
		return false;
	}
	out = mPendingRelays.front();
	mPendingRelays.erase(mPendingRelays.begin());
	return true;
}

void DistributedGameServer::ServerWorldManager::GetOverlappedServers(const Maths::Vector3& origin,
	float radius, std::vector<int>& outServerIDs) const {
	outServerIDs.clear();
	if (radius <= 0.0f || mServerBorderMap == nullptr) {
		return;
	}

	for (const auto& entry : *mServerBorderMap) {
		if (entry.first == mServerID || entry.second == nullptr) {
			continue;   // Excludes this server, per the interface contract.
		}
		const PhysicsServerBorderData* border = entry.second;

		// Closest point on the region rectangle to the sphere centre; inside the
		// radius means the sphere overlaps that region.
		const float closestX = std::clamp(origin.x, border->minXVal, border->maxXVal);
		const float closestZ = std::clamp(origin.z, border->minZVal, border->maxZVal);
		const float dx = origin.x - closestX;
		const float dz = origin.z - closestZ;

		if ((dx * dx + dz * dz) <= (radius * radius)) {
			outServerIDs.push_back(entry.first);
		}
	}
}
```

- [x] **Step 5: Verify the physics API names**

`ApplyLinearImpulse` and `AddForce` must exist on `PhysicsObject`. Check before building:

```powershell
Select-String -Path CSC8503CoreClasses\PhysicsObject.h -Pattern "ApplyLinearImpulse|AddForce"
```

If either is named differently, use the actual name — do not add a wrapper.

- [x] **Step 6: Build the server role**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:EntryPointServer /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
```

Expected: clean build. A pure-virtual error means a method in the `ICommandContext` list was
missed.

- [x] **Step 7: Commit**

```bash
git add DistributedGameServer/ServerWorldManager.h DistributedGameServer/ServerWorldManager.cpp
git commit -m "feat(server): implement command context on world manager"
```

---

### Task 5: Server-side dispatch, dedupe, ack and relay

**Files:**
- Modify: `DistributedGameServer/DistributedGameServerManager.h`
- Modify: `DistributedGameServer/DistributedGameServerManager.cpp`

**Interfaces:**
- Consumes: packets from Task 2, `CommandRegistry` from Task 1, `ICommandContext` from Task 4.
- Produces: handlers for `DistributedClientCommand` and `DistributedServerCommandRelay`; per-player and per-origin dedupe; `@@STAT` counters `cmdApplied`, `cmdRelayed`, `cmdDup`, `cmdRejected`.

- [x] **Step 1: Add the state**

In `DistributedGameServerManager.h`, include `SequenceWindow.h` and add to the protected section:

```cpp
			// (playerID, sequence) is the dedupe key for client commands. Per-player,
			// because sequences are client-scoped and two clients will collide.
			std::map<int, NCL::SequenceWindow> mClientCommandWindows;

			// (originServerID, originSequence) for relays. A relayed area effect can
			// reach one server through two different neighbours; without this an
			// object inside both radii is pushed twice.
			std::map<int, NCL::SequenceWindow> mRelayWindows;
			int mRelaySequenceCounter = 0;

			int mCommandsApplied = 0;
			int mCommandsRelayed = 0;
			int mCommandsDuplicate = 0;
			int mCommandsRejected = 0;

			void HandleClientCommandPacket(NCL::CSC8503::DistributedClientCommandPacket* packet, int source);
			void HandleServerCommandRelayPacket(NCL::CSC8503::DistributedServerCommandRelayPacket* packet);
			void DispatchCommand(NCL::Interaction::CommandType type,
				const NCL::Interaction::CommandArgs& args, int playerID, int clientSequence);
			void DrainPendingRelays(int playerID, int clientSequence);
			void SendCommandAck(int sequence, int playerID, int targetObjectID,
				NCL::Interaction::CommandResult result, int correctedServerID);
```

- [x] **Step 2: Register the handlers and the defaults**

Where the other `RegisterPacketHandler` calls are made in `DistributedGameServerManager`'s
constructor:

```cpp
	RegisterPacketHandler(DistributedClientCommand, this);
	RegisterPacketHandler(DistributedServerCommandRelay, this);

	// One registration for the process. Adding a new interaction never touches
	// ReceivePacket - that is the point of the registry.
	NCL::Interaction::CommandRegistry::RegisterDefaults();
```

And in `ReceivePacket`'s switch:

```cpp
	case DistributedClientCommand: {
		HandleClientCommandPacket(static_cast<DistributedClientCommandPacket*>(payload), source);
		break;
	}
	case DistributedServerCommandRelay: {
		HandleServerCommandRelayPacket(static_cast<DistributedServerCommandRelayPacket*>(payload));
		break;
	}
```

- [x] **Step 3: Implement the client-command handler**

```cpp
void DistributedGameServerManager::HandleClientCommandPacket(DistributedClientCommandPacket* packet, int source) {
	if (packet == nullptr) {
		return;
	}

	const auto type = static_cast<NCL::Interaction::CommandType>(packet->commandType);
	const NCL::Interaction::IInteractionCommand* command =
		NCL::Interaction::CommandRegistry::Instance().Find(type);
	if (command == nullptr) {
		++mCommandsRejected;
		SendCommandAck(packet->sequence, packet->args.playerID, packet->args.targetObjectID,
			NCL::Interaction::CommandResult::Rejected, -1);
		return;
	}

	// Continuous input is idempotent state, so it is deliberately NOT sequenced -
	// dropping a duplicate axis update would be indistinguishable from dropping a
	// real one and would stall movement.
	const NCL::Interaction::CommandScope scope = command->GetScope(packet->args);
	if (!scope.isContinuous) {
		NCL::SequenceWindow& window = mClientCommandWindows[packet->args.playerID];
		if (!window.Accept(packet->sequence)) {
			++mCommandsDuplicate;
			SendCommandAck(packet->sequence, packet->args.playerID, packet->args.targetObjectID,
				NCL::Interaction::CommandResult::Duplicate, -1);
			return;
		}
	}

	DispatchCommand(type, packet->args, packet->args.playerID, packet->sequence);
}
```

- [x] **Step 4: Implement dispatch, ack and relay drain**

```cpp
void DistributedGameServerManager::DispatchCommand(NCL::Interaction::CommandType type,
	const NCL::Interaction::CommandArgs& args, int playerID, int clientSequence) {
	NCL::Interaction::IInteractionCommand* command =
		NCL::Interaction::CommandRegistry::Instance().Find(type);
	if (command == nullptr) {
		++mCommandsRejected;
		return;
	}

	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr) {
		++mCommandsRejected;
		return;
	}

	const NCL::Interaction::CommandResult result = command->Apply(*worldManager, args);

	switch (result) {
	case NCL::Interaction::CommandResult::Applied:  ++mCommandsApplied;  break;
	case NCL::Interaction::CommandResult::Relayed:  ++mCommandsRelayed;  break;
	default:                                        ++mCommandsRejected; break;
	}

	// Queued during Apply, sent now: a command must never re-enter the network layer
	// from inside a packet handler.
	DrainPendingRelays(playerID, clientSequence);

	// Only the server that APPLIED the command acks the client. A relaying server
	// stays silent so the client gets exactly one ack per command.
	if (result != NCL::Interaction::CommandResult::Relayed) {
		SendCommandAck(clientSequence, playerID, args.targetObjectID, result, -1);
	}
}

void DistributedGameServerManager::SendCommandAck(int sequence, int playerID, int targetObjectID,
	NCL::Interaction::CommandResult result, int correctedServerID) {
	DistributedCommandAckPacket packet(sequence, playerID, targetObjectID,
		static_cast<int>(result), correctedServerID);

	// Broadcast to this server's connected clients, matching how snapshots are sent.
	// The client filters on playerID; a directed per-peer send needs the retained
	// ENetPeer* work that is increment 8.
	if (mDistributedPacketSenderServer != nullptr) {
		mDistributedPacketSenderServer->SendGlobalReliablePacket(packet);
	}
}

void DistributedGameServerManager::DrainPendingRelays(int playerID, int clientSequence) {
	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr) {
		return;
	}

	ServerWorldManager::PendingRelay relay;
	while (worldManager->PopPendingRelay(relay)) {
		DistributedServerCommandRelayPacket packet(
			static_cast<int>(relay.type),
			mGameServerID,
			++mRelaySequenceCounter,
			playerID,
			clientSequence,
			relay.args);

		// Directed send over the existing peer mesh - the same lookup
		// SendTransactionHandshakePacket already does, so no new plumbing.
		for (const auto* connection : mDistributedPhysicsClients) {
			if (connection->serverID == relay.targetServerID && connection->client != nullptr) {
				connection->client->SendReliablePacket(packet);
				break;
			}
		}
	}
}
```

> **Registering the relay handler on the peer link.** `ConnectServerToAnotherGameServer`
> registers only `StartSimulatingObjectInServer` on each outbound `GameClient`. Add
> `client->RegisterPacketHandler(BasicNetworkMessages::DistributedServerCommandRelay, this);`
> next to it, or relays arrive and are silently dropped by the receiver.

- [x] **Step 5: Implement the relay handler**

```cpp
void DistributedGameServerManager::HandleServerCommandRelayPacket(DistributedServerCommandRelayPacket* packet) {
	if (packet == nullptr) {
		return;
	}

	// A relay is never re-relayed. Anything with hops already on it is a routing
	// loop, and dropping it loudly beats letting it circulate.
	if (packet->hopCount > 0) {
		++mCommandsRejected;
		std::cout << "ERROR: dropping relay with hopCount " << packet->hopCount
			<< " from server " << packet->originServerID << " - routing loop.\n";
		return;
	}

	NCL::SequenceWindow& window = mRelayWindows[packet->originServerID];
	if (!window.Accept(packet->originSequence)) {
		++mCommandsDuplicate;
		return;
	}

	DispatchCommand(static_cast<NCL::Interaction::CommandType>(packet->commandType),
		packet->args, packet->playerID, packet->clientSequence);
}
```

- [x] **Step 6: Publish the counters**

Wherever the other `@@STAT` values are set for the game server role, add `cmdApplied`,
`cmdRelayed`, `cmdDup` and `cmdRejected`. These four are what the I4 accounting invariant is
computed from: commands sent by clients must equal applied + relayed + duplicate + rejected,
summed across servers.

- [x] **Step 7: Build the server role**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:EntryPointServer /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
```

- [x] **Step 8: Commit**

```bash
git add DistributedGameServer/DistributedGameServerManager.h DistributedGameServer/DistributedGameServerManager.cpp
git commit -m "feat(server): dispatch, dedupe and relay client commands"
```

---

### Task 6: Client-side routing and sending

**Files:**
- Modify: `CSC8503/DistributedMultiplayerGameScene.h`
- Modify: `CSC8503/DistributedMultiplayerGameScene.cpp`

**Interfaces:**
- Consumes: packets from Task 2; existing `mObjectOwner` and `mServerRegions`.
- Produces: `int ResolveCommandTarget(const CommandArgs&, const CommandScope&) const`; `bool SendCommand(CommandType, const CommandArgs&)`.

- [x] **Step 1: Add the state and methods**

In `DistributedMultiplayerGameScene.h`:

```cpp
	// Returns the serverId a command must be sent to, or -1 if it cannot be resolved.
	int ResolveCommandTarget(const NCL::Interaction::CommandArgs& args,
		const NCL::Interaction::CommandScope& scope) const;

	// Routes and sends. False means the command was rejected locally and never left
	// the machine.
	bool SendCommand(NCL::Interaction::CommandType type, NCL::Interaction::CommandArgs args);
```

Protected:

```cpp
	// One global sequence across every server link, not one per link: (playerID,
	// sequence) must be unique whichever server ends up applying the command.
	int mNextCommandSequence = 1;
	int mCommandsSent = 0;
	std::map<int, int> mAckResultCounts;   // CommandResult -> count, for the I4 invariant
```

- [x] **Step 2: Implement resolution**

```cpp
int DistributedMultiplayerGameScene::ResolveCommandTarget(
	const NCL::Interaction::CommandArgs& args,
	const NCL::Interaction::CommandScope& scope) const {

	if (scope.targetsObject && args.targetObjectID >= 0) {
		const auto owner = mObjectOwner.find(args.targetObjectID);
		if (owner != mObjectOwner.end()) {
			return owner->second;
		}
		// Fall through: the object may be known by position even if the owner table
		// has not seen it yet.
	}

	if (scope.targetsPoint) {
		for (const ServerRegion& region : mServerRegions) {
			// Half-open on both axes, matching the server-side rule. Using a
			// different rule here would misroute every command on a border.
			if (args.worldPoint.x >= region.minX && args.worldPoint.x < region.maxX &&
				args.worldPoint.z >= region.minZ && args.worldPoint.z < region.maxZ) {
				return region.serverId;
			}
		}
	}

	// Deliberately no broadcast fallback: N servers would each apply the command.
	return -1;
}
```

- [x] **Step 3: Implement sending**

```cpp
bool DistributedMultiplayerGameScene::SendCommand(NCL::Interaction::CommandType type,
	NCL::Interaction::CommandArgs args) {

	NCL::Interaction::IInteractionCommand* command =
		NCL::Interaction::CommandRegistry::Instance().Find(type);
	if (command == nullptr) {
		return false;
	}

	// Client-side validation is an optimisation only - the server validates again.
	if (!command->Validate(args)) {
		return false;
	}

	const NCL::Interaction::CommandScope scope = command->GetScope(args);
	const int targetServerId = ResolveCommandTarget(args, scope);
	if (targetServerId < 0) {
		std::cout << "Command dropped: no server resolved for object "
			<< args.targetObjectID << ".\n";
		return false;
	}

	GameClient* link = nullptr;
	for (const PhysicsServerLink& serverLink : mDistributedPhysicsClients) {
		if (serverLink.serverId == targetServerId) {
			link = serverLink.client;
			break;
		}
	}
	if (link == nullptr) {
		return false;
	}

	// Continuous input is state and is re-sent every tick, so it is not sequenced;
	// giving it a sequence would consume the dedupe window in a few seconds.
	const int sequence = scope.isContinuous ? 0 : mNextCommandSequence++;

	DistributedClientCommandPacket packet(static_cast<int>(type), sequence, targetServerId, args);
	link->SendReliablePacket(packet);
	++mCommandsSent;
	return true;
}
```

- [x] **Step 4: Handle the ack**

Register `DistributedCommandAck` in the scene's handler registration, and in `ReceivePacket`:

```cpp
	case DistributedCommandAck: {
		auto* ack = static_cast<DistributedCommandAckPacket*>(payload);
		++mAckResultCounts[ack->result];

		// A NotOwner ack carries the true owner. Adopting it immediately is what
		// stops a stale owner table from misrouting every subsequent command for
		// this object.
		if (ack->result == static_cast<int>(NCL::Interaction::CommandResult::NotOwner) &&
			ack->correctedServerID >= 0 && ack->targetObjectID >= 0) {
			mObjectOwner[ack->targetObjectID] = ack->correctedServerID;
		}
		break;
	}
```

> Acks are broadcast to every client on that server (Task 5), so a client must ignore acks whose
> `playerID` is not its own before counting them — otherwise the I4 tally double-counts.

- [x] **Step 5: Build the client**

The client is the non-distributed configure, so it needs its own:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Debug
```

- [x] **Step 6: Commit**

```bash
git add CSC8503/DistributedMultiplayerGameScene.h CSC8503/DistributedMultiplayerGameScene.cpp CSC8503CoreClasses/NetworkObject.h CSC8503CoreClasses/NetworkObject.cpp
git commit -m "feat(client): route and send interaction commands"
```

---

### Task 7: End-to-end verification

**Files:** none — this task is measurement.

- [x] **Step 1: Run the full unit suite**

```powershell
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: every test from increments 1 and 3 passes, exit code 0.

- [x] **Step 2: Run a two-server scenario with commands**

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
powershell -ExecutionPolicy Bypass -File tools\measure.ps1 -Servers 2 -Objects 400 -Seconds 60 -Tag s2-o400-cmd
```

- [x] **Step 3: Check the I4 accounting invariant**

From the run logs, sum across both servers:

```
cmdApplied + cmdRelayed + cmdDup + cmdRejected  ==  commands sent by the client
```

A shortfall means commands are being silently swallowed — the failure mode this counter exists to
catch. Note that a relayed command contributes `Relayed` on the first server **and** `Applied` on
the second, so the identity to check is: `sent == applied + rejected + dup` with `relayed`
accounted separately as an internal hop.

- [x] **Step 4: Confirm the handoff baseline is unchanged**

Compare `hoSent`/`hoRecv`/`hoFail` and final object counts against the increment 1 run
(`s2-o400-postreg`). This increment is purely additive to the handoff protocol; any change is a
defect.

- [x] **Step 5: Record implementation notes**

Append to the spec's implementation-notes section: what shipped, that `SpawnObject`/`DestroyObject`
are stubs pending increments 5–6, whether the directed peer send existed or the broadcast path was
reused (Task 5 Step 4), and the I4 numbers from Step 3.

- [x] **Step 6: Commit**

```bash
git add docs/superpowers/specs/2026-08-16-interactions-toolset-design.md
git commit -m "docs: record increment 3 verification results"
```

---

## Out of scope for this plan

- **Cross-border area effects** (increment 4). `ApplyRadialImpulse` and `GetOverlappedServers` are
  implemented here because `ICommandContext` requires them, but no command uses a radius yet.
- **Runtime spawn and destroy** (increments 5–6). `SpawnObject`/`DestroyObject` are honest stubs.
- **Player-controlled avatars** (increment 7). `MoveAxis` pushes an existing object; it does not
  add a controller ID to `TestObject` or extend the handoff packet.
- **The `--scenario` replay flag** (spec §7.3 Tier 1). Task 7 verifies by hand; scripted replay
  arrives with the evaluation harness.

---

## Execution outcome (2026-08-16)

All seven tasks completed. Deviations:

- **`BasicNetworkMessages` and `GamePacket` are at global scope**, not `NCL::` / `NCL::CSC8503::`.
  The plan's test snippets qualified them wrongly.
- **The directed peer send already existed** (`mDistributedPhysicsClients` +
  `SendTransactionHandshakePacket`), so the plan's hedge about reusing the broadcast path was
  unnecessary. It did need the relay type registering on the outbound peer link.
- **`DistributedCommandAckPacket` gained `targetObjectID`** as the plan anticipated, so the client
  can correct `mObjectOwner` on a `NotOwner` ack.
- **`RegisterDefaults()` also has to run on the client**, which the plan did not mention.

Work added beyond the plan, all of it needed to make Task 7 checkable at all:

- `--impulse-test N` on the client — a headless client has no input path, so without a driver the
  channel was wired but never exercised and `cmdApplied` stayed 0.
- `--misroute-every N` and `SendCommandTo(..., forcedServerId)` — the natural staleness window is
  milliseconds wide, so the relay path had to be forced.
- Exact `@@FINAL` totals on both roles plus a bounded client run, because 2 Hz sampling can never
  make the two ends of the I4 tally align.
- **A real bug fix:** peer links were labelled by array index rather than server id, so every
  relay was silently dropped — and the same lookup is used by `SendTransactionHandshakePacket`.
  See §11 of the spec.

Still deliberately out of scope, as planned: cross-border area effects (increment 4), runtime
spawn/destroy (5–6, `SpawnObject`/`DestroyObject` are honest stubs), player avatars (7), late-join
manifest (8), and the `--scenario` replay flag.
