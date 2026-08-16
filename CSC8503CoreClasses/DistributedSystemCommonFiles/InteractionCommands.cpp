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
