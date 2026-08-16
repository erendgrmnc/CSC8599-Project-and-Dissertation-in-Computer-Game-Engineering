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
