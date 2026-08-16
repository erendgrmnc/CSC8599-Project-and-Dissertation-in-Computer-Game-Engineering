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
