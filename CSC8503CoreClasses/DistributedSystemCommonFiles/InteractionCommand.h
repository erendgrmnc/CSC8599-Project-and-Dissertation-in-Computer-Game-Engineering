#pragma once
#include <map>
#include <memory>
#include <vector>

#include "Vector3.h"

// Deliberately free of USEGL / DISTRIBUTEDSYSTEMACTIVE guards: this header is
// included by the three server roles, by the client (which does NOT define
// DISTRIBUTEDSYSTEMACTIVE), and by the Tier 0 test target. A guard here would make
// the packet layouts in NetworkObject.h differ between roles.

namespace NCL::CSC8503 { class GameObject; }

namespace NCL::Interaction {

	// Wire values, APPEND ONLY: these travel inside DistributedClientCommandPacket.
	// The three server roles and the client are built and deployed separately, so a
	// renumber here is a silent cross-version misinterpretation, not a build error.
	enum class CommandType : int {
		None      = 0,
		MoveAxis  = 1,   // continuous, state-like: not sequenced, never relayed
		Impulse   = 2,
		Spawn     = 3,
		Destroy   = 4,
		Grab      = 5,
		Teleport  = 6
	};

	enum class CommandResult : int {
		Applied = 0,
		Relayed,             // forwarded to the true owner; that server acks separately
		NotOwner,            // owner changed again; correctedServerID is populated
		ObjectUnknown,
		ObjectDestroyed,
		Duplicate,
		Rejected
	};

	enum class DespawnReason : int { Destroyed = 0, LeftWorld = 1 };

	// Bit flags carried in CommandArgs::flags. Wire values - APPEND ONLY.
	enum class CommandFlags : int {
		None = 0,
		// Set by the origin server when it fans an area effect out to overlapped
		// regions. A receiver applies it locally but must not fan out again, or one
		// blast would circulate around the peer mesh forever.
		AlreadyFannedOut = 1 << 0
	};

	// POD payload shared by every command type. Fixed size; no std::string, no
	// std::vector, no pointers: the ENet path memcpys these structs verbatim
	// (GameClient::SendPacket), which is why DistributedClientConnectToPhysicsServerPacket
	// uses a char borderStr[256] rather than a std::string.
	struct CommandArgs {
		int            targetObjectID = -1;   // -1 = none
		int            playerID       = -1;
		int            archetypeID    = 0;    // Spawn: which prefab
		int            flags          = 0;
		Maths::Vector3 worldPoint;            // spawn point / effect origin / teleport destination
		Maths::Vector3 direction;             // impulse direction or movement axis
		float          magnitude      = 0.0f;
		float          radius         = 0.0f; // > 0 => area effect, may cross region borders
	};

	// Where a command must execute. Derived from the args by the command ITSELF, so
	// the routing layer never switches on CommandType.
	struct CommandScope {
		bool targetsObject = false;  // owner = current owner of targetObjectID
		bool targetsPoint  = false;  // owner = server whose region contains worldPoint
		bool isAreaEffect  = false;  // additionally relay to every region the radius overlaps
		bool isContinuous  = false;  // state, not an event: never sequenced, never relayed
	};

	class ICommandContext;

	class IInteractionCommand {
	public:
		virtual ~IInteractionCommand() = default;

		virtual CommandType  GetType() const = 0;
		virtual CommandScope GetScope(const CommandArgs& args) const = 0;

		// Cheap, side-effect free. Run on the client before routing AND on the server
		// before Apply - the client-side call is an optimisation only, never a
		// substitute for the server-side one.
		virtual bool Validate(const CommandArgs& args) const { return true; }

		// Executed on the authoritative server only, inside the network pump, before
		// ServerWorldManager::Update for that tick.
		virtual CommandResult Apply(ICommandContext& ctx, const CommandArgs& args) = 0;
	};

	// Everything a command is allowed to do. Implemented by ServerWorldManager, so
	// commands never see the network layer and the network layer never sees the world.
	class ICommandContext {
	public:
		virtual ~ICommandContext() = default;

		virtual int GetServerID() const = 0;

		// The SINGLE source of truth for ownership. -1 outside the world.
		virtual int GetOwningServer(const Maths::Vector3& worldPoint) const = 0;

		// Active on THIS server only; null if unknown, destroyed, or handed off.
		virtual CSC8503::GameObject* FindActiveObject(int networkObjectID) const = 0;

		// Last known transform for any object in the pool, active or not. Lets a
		// non-owner resolve where an object-targeted command should be relayed.
		virtual bool TryGetLastKnownPosition(int networkObjectID, Maths::Vector3& out) const = 0;

		// Returns the allocated networkID, or -1 on failure (ID space exhausted).
		virtual int  SpawnObject(int archetypeID, const Maths::Vector3& at, int spawnerPlayerID) = 0;
		virtual bool DestroyObject(int networkObjectID, DespawnReason reason, int destroyerPlayerID) = 0;

		virtual void ApplyImpulse(int networkObjectID, const Maths::Vector3& impulse) = 0;
		virtual void ApplyRadialImpulse(const Maths::Vector3& origin, float radius, float magnitude) = 0;
		virtual void SetMoveAxis(int networkObjectID, int playerID, const Maths::Vector3& axis) = 0;

		// Queued, not sent: the manager drains the queue after Apply returns, so a
		// command never blocks inside the network layer or re-enters it.
		virtual void RelayToServer(int serverID, CommandType type, const CommandArgs& args) = 0;

		// Every region the sphere overlaps, EXCLUDING this server. Backed by the
		// mServerBorderMap every game server already holds.
		virtual void GetOverlappedServers(const Maths::Vector3& origin, float radius,
			std::vector<int>& outServerIDs) const = 0;
	};

	// One instance per process. Registered once at startup; adding a new interaction
	// type never touches DistributedGameServerManager::ReceivePacket.
	class CommandRegistry {
	public:
		static CommandRegistry& Instance();

		void Register(std::unique_ptr<IInteractionCommand> command);
		IInteractionCommand* Find(CommandType type) const;

		// Registers every built-in interaction. RegisterDefaults() fills the process
		// singleton; RegisterDefaultsInto() exists so tests can use a local registry
		// and stay independent of one another.
		static void RegisterDefaults();
		static void RegisterDefaultsInto(CommandRegistry& registry);

	protected:
		std::map<CommandType, std::unique_ptr<IInteractionCommand>> mCommands;
	};
}
