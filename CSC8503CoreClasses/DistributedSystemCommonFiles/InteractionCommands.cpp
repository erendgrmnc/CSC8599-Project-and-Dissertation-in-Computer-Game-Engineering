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

		// Where to forward an object-targeted command this server could not apply.
		//
		// Position first: while a server still holds a copy of an object it does not
		// own - a pre-seeded twin today, a halo shadow later - that position lies in
		// the real owner's region and is the most current answer available.
		//
		// Then the forwarding table, for objects this server holds nothing for. A
		// server that has handed an object away has no position for it, so without
		// this the command would be rejected as ObjectUnknown and race W2 would
		// reopen.
		bool ResolveForwardTarget(const ICommandContext& ctx, int objectID, int& outServerID) {
			Maths::Vector3 lastKnown;
			if (ctx.TryGetLastKnownPosition(objectID, lastKnown)) {
				const int owner = ctx.GetOwningServer(lastKnown);
				if (owner >= 0 && owner != ctx.GetServerID()) {
					outServerID = owner;
					return true;
				}
				// Deliberately falls through rather than returning false. A position
				// that maps back to US for an object that is not active here means we
				// handed it away and are holding a stale copy; the table knows where
				// it actually went.
			}

			int recorded = -1;
			if (ctx.TryGetLastKnownOwner(objectID, recorded)
				&& recorded >= 0 && recorded != ctx.GetServerID()) {
				outServerID = recorded;
				return true;
			}
			return false;
		}

		// A one-shot push on a single object. Object-targeted, so the owner is
		// whichever server currently has it active.
		class ImpulseCommand : public IInteractionCommand {
		public:
			CommandType GetType() const override { return CommandType::Impulse; }

			// A radius turns this from a push on one object into a field centred on a
			// point, so it routes by point and may cross region borders.
			CommandScope GetScope(const CommandArgs& args) const override {
				CommandScope scope;
				if (args.radius > 0.0f) {
					scope.targetsPoint = true;
					scope.isAreaEffect = true;
				}
				else {
					scope.targetsObject = true;
				}
				return scope;
			}

			bool Validate(const CommandArgs& args) const override {
				if (args.magnitude <= 0.0f) {
					return false;
				}

				// An area effect is defined by its origin and radius; it needs neither
				// a target object nor a direction, since the direction is radial.
				if (args.radius > 0.0f) {
					return true;
				}

				if (args.targetObjectID < 0) {
					return false;
				}
				const Maths::Vector3 direction = NormalisedOrZero(args.direction);
				return !(direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f);
			}

			CommandResult Apply(ICommandContext& ctx, const CommandArgs& args) override {
				if (!Validate(args)) {
					return CommandResult::Rejected;
				}

				if (args.radius > 0.0f) {
					return ApplyArea(ctx, args);
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
				int owner = -1;
				if (!ResolveForwardTarget(ctx, args.targetObjectID, owner)) {
					return CommandResult::ObjectUnknown;
				}

				ctx.RelayToServer(owner, GetType(), args);
				return CommandResult::Relayed;
			}

		private:
			// Server-to-server event relay, not a ghost band: each server applies the
			// field only to objects IT owns, so ownership stays exactly where the
			// handoff protocol put it and nothing here perturbs that protocol.
			CommandResult ApplyArea(ICommandContext& ctx, const CommandArgs& args) const {
				ctx.ApplyRadialImpulse(args.worldPoint, args.radius, args.magnitude);

				// A relayed blast has already been fanned out by its origin. Fanning
				// out again would circulate it around the mesh; objects in a doubly
				// overlapped region would also be pushed twice, which reads as 2x
				// velocity and silently corrupts any measurement.
				const bool alreadyFannedOut =
					(args.flags & static_cast<int>(CommandFlags::AlreadyFannedOut)) != 0;
				if (alreadyFannedOut) {
					return CommandResult::Applied;
				}

				std::vector<int> overlapped;
				ctx.GetOverlappedServers(args.worldPoint, args.radius, overlapped);

				CommandArgs fanned = args;
				fanned.flags |= static_cast<int>(CommandFlags::AlreadyFannedOut);
				for (int serverID : overlapped) {
					ctx.RelayToServer(serverID, GetType(), fanned);
				}

				// Applied, not Relayed: this server DID do the work for its own
				// objects. Reporting Relayed would suppress the ack and unbalance I4.
				return CommandResult::Applied;
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

		// Creates an object at a point. Point-targeted, so the owner is whichever
		// server's region contains the spawn point - spawn is NOT a special case in
		// the routing layer, and a spawn exactly on a border is simply owned by
		// whoever the half-open rule assigns it to.
		class SpawnCommand : public IInteractionCommand {
		public:
			CommandType GetType() const override { return CommandType::Spawn; }

			CommandScope GetScope(const CommandArgs&) const override {
				CommandScope scope;
				scope.targetsPoint = true;
				return scope;
			}

			bool Validate(const CommandArgs& args) const override {
				return args.archetypeID >= 0;
			}

			CommandResult Apply(ICommandContext& ctx, const CommandArgs& args) override {
				if (!Validate(args)) {
					return CommandResult::Rejected;
				}

				const int owner = ctx.GetOwningServer(args.worldPoint);
				if (owner < 0) {
					return CommandResult::Rejected;   // Outside the world entirely.
				}
				if (owner != ctx.GetServerID()) {
					ctx.RelayToServer(owner, GetType(), args);
					return CommandResult::Relayed;
				}

				const int spawnedID = ctx.SpawnObject(args.archetypeID, args.worldPoint, args.playerID);
				// -1 means the partitioned id space is exhausted. Reporting it as
				// rejected keeps the I4 tally honest instead of losing the command.
				return (spawnedID >= 0) ? CommandResult::Applied : CommandResult::Rejected;
			}
		};

		// Destroys an object. Object-targeted, so it follows the object rather than a
		// point - and that is also what covers race W2 without any protocol change:
		// a server that has already handed the object away still holds its last known
		// position, which by definition lies in the NEW owner's region, so the normal
		// relay-on-not-owner path forwards the destroy to exactly the right server.
		class DestroyCommand : public IInteractionCommand {
		public:
			CommandType GetType() const override { return CommandType::Destroy; }

			CommandScope GetScope(const CommandArgs&) const override {
				CommandScope scope;
				scope.targetsObject = true;
				return scope;
			}

			bool Validate(const CommandArgs& args) const override {
				return args.targetObjectID >= 0;
			}

			CommandResult Apply(ICommandContext& ctx, const CommandArgs& args) override {
				if (!Validate(args)) {
					return CommandResult::Rejected;
				}

				if (ctx.FindActiveObject(args.targetObjectID) != nullptr) {
					const bool destroyed = ctx.DestroyObject(args.targetObjectID,
						DespawnReason::Destroyed, args.playerID);
					return destroyed ? CommandResult::Applied : CommandResult::ObjectUnknown;
				}

				// Not active here. It may have been handed off, or already destroyed.
				// Order matters: a tombstone must report ObjectDestroyed rather than
				// being forwarded, so the destroyed case is tested before forwarding.
				int owner = -1;
				if (!ResolveForwardTarget(ctx, args.targetObjectID, owner)) {
					// Nothing held, nothing recorded: there is nothing left to destroy
					// and nowhere to forward to.
					return CommandResult::ObjectDestroyed;
				}

				ctx.RelayToServer(owner, GetType(), args);
				return CommandResult::Relayed;
			}
		};
	}

	void CommandRegistry::RegisterDefaultsInto(CommandRegistry& registry) {
		registry.Register(std::make_unique<ImpulseCommand>());
		registry.Register(std::make_unique<MoveAxisCommand>());
		registry.Register(std::make_unique<SpawnCommand>());
		registry.Register(std::make_unique<DestroyCommand>());
	}

	void CommandRegistry::RegisterDefaults() {
		RegisterDefaultsInto(Instance());
	}
}
