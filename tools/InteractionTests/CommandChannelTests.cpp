#include "TestHarness.h"

#include "DistributedSystemCommonFiles/InteractionCommand.h"
#include "DistributedSystemCommonFiles/SequenceWindow.h"
#include "NetworkObject.h"

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

// The size field convention is size = sizeof(T) - sizeof(GamePacket). Getting this
// wrong truncates the payload on the wire and only shows up across machines.
TEST(ClientCommandPacketHasCorrectLayout) {
	CHECK(std::is_trivially_copyable_v<DistributedClientCommandPacket>);

	CommandArgs args;
	args.targetObjectID = 7;
	args.magnitude = 12.5f;

	DistributedClientCommandPacket packet(
		static_cast<int>(CommandType::Impulse), 3, 1, args);

	CHECK_EQ(packet.type, (short)BasicNetworkMessages::DistributedClientCommand);
	CHECK_EQ((size_t)packet.size + sizeof(GamePacket),
		sizeof(DistributedClientCommandPacket));
	CHECK_EQ(packet.commandType, (int)CommandType::Impulse);
	CHECK_EQ(packet.sequence, 3);
	CHECK_EQ(packet.hintServerID, 1);
	CHECK_EQ(packet.args.targetObjectID, 7);
	CHECK_NEAR(packet.args.magnitude, 12.5f, 1e-6);
}

TEST(CommandAckPacketHasCorrectLayout) {
	CHECK(std::is_trivially_copyable_v<DistributedCommandAckPacket>);

	DistributedCommandAckPacket packet(
		9, 2, 42, static_cast<int>(CommandResult::NotOwner), 1);

	CHECK_EQ(packet.type, (short)BasicNetworkMessages::DistributedCommandAck);
	CHECK_EQ((size_t)packet.size + sizeof(GamePacket),
		sizeof(DistributedCommandAckPacket));
	CHECK_EQ(packet.sequence, 9);
	CHECK_EQ(packet.playerID, 2);
	CHECK_EQ(packet.targetObjectID, 42);
	CHECK_EQ(packet.result, (int)CommandResult::NotOwner);
	CHECK_EQ(packet.correctedServerID, 1);
}

TEST(ServerCommandRelayPacketHasCorrectLayout) {
	CHECK(std::is_trivially_copyable_v<DistributedServerCommandRelayPacket>);

	CommandArgs args;
	args.targetObjectID = 42;

	DistributedServerCommandRelayPacket packet(
		static_cast<int>(CommandType::Impulse), 0, 5, 2, 9, args);

	CHECK_EQ(packet.type, (short)BasicNetworkMessages::DistributedServerCommandRelay);
	CHECK_EQ((size_t)packet.size + sizeof(GamePacket),
		sizeof(DistributedServerCommandRelayPacket));
	CHECK_EQ(packet.originServerID, 0);
	CHECK_EQ(packet.originSequence, 5);
	CHECK_EQ(packet.playerID, 2);
	CHECK_EQ(packet.clientSequence, 9);
	// A relay must never be re-relayed; receiving one with hopCount > 0 is a bug.
	CHECK_EQ(packet.hopCount, 0);
	CHECK_EQ(packet.args.targetObjectID, 42);
}

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
	CommandRegistry::RegisterDefaultsInto(registry);
	CHECK(registry.Find(CommandType::MoveAxis) != nullptr);
	CHECK(registry.Find(CommandType::Impulse) != nullptr);
}

TEST(ImpulseAppliesToOwnedObject) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);
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
	CommandRegistry::RegisterDefaultsInto(registry);
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
	CommandRegistry::RegisterDefaultsInto(registry);
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
	CommandRegistry::RegisterDefaultsInto(registry);
	IInteractionCommand* impulse = registry.Find(CommandType::Impulse);

	CommandArgs args;
	args.targetObjectID = 5;
	args.direction = Maths::Vector3(0, 0, 0);
	args.magnitude = 10.0f;

	CHECK(!impulse->Validate(args));
}

TEST(ImpulseScopeTargetsObject) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	CommandArgs args;
	args.targetObjectID = 5;
	const CommandScope scope = registry.Find(CommandType::Impulse)->GetScope(args);

	CHECK(scope.targetsObject);
	CHECK(!scope.isContinuous);
}

// MoveAxis is continuous state, not an event: it must never be sequenced or relayed.
TEST(MoveAxisScopeIsContinuous) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	CommandArgs args;
	args.targetObjectID = 5;
	const CommandScope scope = registry.Find(CommandType::MoveAxis)->GetScope(args);

	CHECK(scope.isContinuous);
	CHECK(scope.targetsObject);
}

TEST(MoveAxisAppliesToOwnedObject) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

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
	CommandRegistry::RegisterDefaultsInto(registry);

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

// --- Area effects (cross-border) ------------------------------------------

namespace {
	// Extends FakeContext with region overlap, so an area effect can be tested
	// without any real border map.
	class AreaFakeContext : public FakeContext {
	public:
		std::vector<int> overlappedServers;

		struct RadialCall { Maths::Vector3 origin; float radius; float magnitude; };
		std::vector<RadialCall> radials;

		void ApplyRadialImpulse(const Maths::Vector3& origin, float radius, float magnitude) override {
			radials.push_back({ origin, radius, magnitude });
		}
		void GetOverlappedServers(const Maths::Vector3&, float, std::vector<int>& out) const override {
			out = overlappedServers;
		}
	};
}

// A radius turns the command from object-targeted into a point-targeted area
// effect. Routing must follow the point, not an object id.
TEST(ImpulseWithRadiusIsAnAreaEffect) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	CommandArgs args;
	args.worldPoint = Maths::Vector3(0, 0, 0);
	args.direction = Maths::Vector3(1, 0, 0);
	args.magnitude = 10.0f;
	args.radius = 25.0f;

	const CommandScope scope = registry.Find(CommandType::Impulse)->GetScope(args);
	CHECK(scope.isAreaEffect);
	CHECK(scope.targetsPoint);
	CHECK(!scope.targetsObject);
}

// An area effect needs no target object, so the object-id validation that a
// point impulse requires must not apply to it.
TEST(AreaEffectValidatesWithoutATargetObject) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	CommandArgs args;
	args.targetObjectID = -1;
	args.worldPoint = Maths::Vector3(0, 0, 0);
	args.magnitude = 10.0f;
	args.radius = 25.0f;

	CHECK(registry.Find(CommandType::Impulse)->Validate(args));
}

// The core of section 5.1: apply locally to owned objects, and relay to every
// region the sphere overlaps. No server ever writes to an object it does not own.
TEST(AreaEffectAppliesLocallyAndRelaysToOverlappedRegions) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	AreaFakeContext ctx;
	ctx.serverID = 0;
	ctx.overlappedServers = { 1, 2 };

	CommandArgs args;
	args.worldPoint = Maths::Vector3(0, 0, 0);
	args.magnitude = 10.0f;
	args.radius = 25.0f;

	const CommandResult result = registry.Find(CommandType::Impulse)->Apply(ctx, args);

	CHECK(result == CommandResult::Applied);
	CHECK_EQ((int)ctx.radials.size(), 1);
	if (!ctx.radials.empty()) {
		CHECK_NEAR(ctx.radials[0].radius, 25.0f, 1e-5);
	}
	CHECK_EQ((int)ctx.relays.size(), 2);
}

// A relayed area effect must apply locally and NOT fan out again, or one blast
// would circulate around the mesh.
TEST(AreaEffectDoesNotRelayWhenAlreadyRelayed) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	AreaFakeContext ctx;
	ctx.serverID = 1;
	ctx.overlappedServers = { 0, 2 };

	CommandArgs args;
	args.worldPoint = Maths::Vector3(0, 0, 0);
	args.magnitude = 10.0f;
	args.radius = 25.0f;
	args.flags = static_cast<int>(CommandFlags::AlreadyFannedOut);

	const CommandResult result = registry.Find(CommandType::Impulse)->Apply(ctx, args);

	CHECK(result == CommandResult::Applied);
	CHECK_EQ((int)ctx.radials.size(), 1);
	CHECK_EQ((int)ctx.relays.size(), 0);
}

// An area effect with nothing nearby still counts as applied - it did what it
// was asked to do. Reporting it as rejected would break the I4 tally.
TEST(AreaEffectWithNoOverlapStillApplies) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	AreaFakeContext ctx;
	ctx.overlappedServers = {};

	CommandArgs args;
	args.worldPoint = Maths::Vector3(0, 0, 0);
	args.magnitude = 10.0f;
	args.radius = 25.0f;

	CHECK(registry.Find(CommandType::Impulse)->Apply(ctx, args) == CommandResult::Applied);
	CHECK_EQ((int)ctx.relays.size(), 0);
}

// --- Destroy ---------------------------------------------------------------

namespace {
	class DestroyFakeContext : public FakeContext {
	public:
		std::vector<int> destroyed;
		bool destroySucceeds = true;

		bool DestroyObject(int objectID, DespawnReason, int) override {
			destroyed.push_back(objectID);
			return destroySucceeds;
		}
	};
}

TEST(DestroyAppliesToOwnedObject) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	DestroyFakeContext ctx;
	ctx.objectIsActiveHere = true;

	CommandArgs args;
	args.targetObjectID = 7;

	CHECK(registry.Find(CommandType::Destroy)->Apply(ctx, args) == CommandResult::Applied);
	CHECK_EQ((int)ctx.destroyed.size(), 1);
}

// Race W2 without any protocol change: a server that already handed the object
// away still holds its last known position, which lies in the NEW owner's region,
// so the ordinary relay path forwards the destroy to exactly the right server.
TEST(DestroyRelaysAfterHandoff) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	DestroyFakeContext ctx;
	ctx.serverID = 0;
	ctx.objectIsActiveHere = false;
	ctx.hasLastKnownPosition = true;
	ctx.lastKnownPosition = Maths::Vector3(80, 0, 0);
	ctx.owningServerResult = 1;

	CommandArgs args;
	args.targetObjectID = 7;

	CHECK(registry.Find(CommandType::Destroy)->Apply(ctx, args) == CommandResult::Relayed);
	CHECK_EQ((int)ctx.relays.size(), 1);
	CHECK_EQ((int)ctx.destroyed.size(), 0);
}

// A tombstoned entry reports no last known position, so the destroy resolves to
// ObjectDestroyed - a materially more useful ack than ObjectUnknown, and it keeps
// the I4 tally balanced for a duplicate destroy (race W4).
TEST(DestroyOnTombstoneReportsAlreadyDestroyed) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	DestroyFakeContext ctx;
	ctx.objectIsActiveHere = false;
	ctx.hasLastKnownPosition = false;

	CommandArgs args;
	args.targetObjectID = 7;

	CHECK(registry.Find(CommandType::Destroy)->Apply(ctx, args) == CommandResult::ObjectDestroyed);
	CHECK_EQ((int)ctx.relays.size(), 0);
}

TEST(DestroyRejectsMissingTarget) {
	CommandRegistry registry;
	CommandRegistry::RegisterDefaultsInto(registry);

	CommandArgs args;
	args.targetObjectID = -1;

	CHECK(!registry.Find(CommandType::Destroy)->Validate(args));
}
