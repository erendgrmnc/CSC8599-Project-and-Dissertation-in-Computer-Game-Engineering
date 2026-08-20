#include "DistributedSystemCommonFiles/DistributedPhysicsServerDto.h"
#ifdef USEGL
#pragma once
#include "GameObject.h"
#include "NetworkBase.h"
#include "NetworkState.h"
#include "GameClient.h"
#include "DistributedSystemCommonFiles/InteractionCommand.h"

#include <type_traits>

// Distributed manager/server/world headers reference NCL::CSC8503 packet and
// object types unqualified; this directive (previously pulled in transitively
// via the removed team-game include chain) keeps that name lookup working.
using namespace NCL::CSC8503;

namespace NCL::CSC8503 {
	class GameObject;

	struct FullPacket : public GamePacket {
		int		objectID = -1;
		int serverID = -1;
		NetworkState fullState;

		FullPacket() {
			type = Full_State;
			size = sizeof(FullPacket) - sizeof(GamePacket);
		}
	};

	struct DeltaPacket : public GamePacket {
		int		fullID = -1;
		int		objectID = -1;
		int serverID = -1;
		char	pos[3];
		char	orientation[4];

		DeltaPacket() {
			type = Delta_State;
			size = sizeof(DeltaPacket) - sizeof(GamePacket);
		}
	};

	// Client -> game server acknowledgement of snapshot progress. Sent whenever the
	// client applies a full snapshot; the server keeps the minimum acked ID across
	// all connected clients as the delta baseline and prunes state history to it.
	struct DistributedClientSnapshotAckPacket : public GamePacket {
		int lastFullStateID = -1;
		int gameServerID = -1;

		DistributedClientSnapshotAckPacket(int lastFullStateID, int gameServerID) {
			type = DistributedClientSnapshotAck;
			size = sizeof(DistributedClientSnapshotAckPacket) - sizeof(GamePacket);
			this->lastFullStateID = lastFullStateID;
			this->gameServerID = gameServerID;
		}
	};

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
	// Turns a future std::string member into a compile error rather than a wire
	// corruption that only manifests across machines.
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

	// Owning game server -> peers (which build a DEACTIVATED pool entry, mirroring
	// the pre-seed model) and clients (which build a replica). Reliable, broadcast on
	// the packet-sender server, exactly as the handoff packet already is.
	//
	// Carries an archetype ID rather than a description: every role must build a
	// byte-identical object, and a description would mean a variable-length string
	// inside a struct that is memcpy'd onto the wire.
	struct DistributedObjectSpawnedPacket : public GamePacket {
		int objectID;                 // from NetworkIdSpace::MakeRuntimeId
		int archetypeID;              // NCL::Interaction::ObjectArchetype
		int ownerServerID;
		int spawnerPlayerID;          // -1 for system spawns
		Vector3 position;

		DistributedObjectSpawnedPacket(int objectID, int archetypeID, int ownerServerID,
			int spawnerPlayerID, const Vector3& position);
	};
	static_assert(std::is_trivially_copyable_v<DistributedObjectSpawnedPacket>);

	// Owning game server -> peers and clients. An EXPLICIT despawn rather than
	// "absence from a snapshot": absence already means "not mine", so overloading it
	// would make a destroyed object indistinguishable from a handed-off one.
	struct DistributedObjectDespawnedPacket : public GamePacket {
		int objectID;
		int reason;                   // NCL::Interaction::DespawnReason
		int destroyerPlayerID;        // -1 for system despawns

		DistributedObjectDespawnedPacket(int objectID, int reason, int destroyerPlayerID);
	};
	static_assert(std::is_trivially_copyable_v<DistributedObjectDespawnedPacket>);

	// One object's state as a NEIGHBOURING server needs to see it, so that objects
	// either side of a region border can collide.
	//
	// Everything a shadow needs to be built and to take part in contact resolution,
	// and nothing else: no force, no torque, no accumulated impulse. A shadow is never
	// integrated, so the terms that only matter to integration would be dead weight on
	// a message sent every tick to every neighbour.
	struct HaloObjectState {
		int objectID;
		int archetypeID;              // NCL::Interaction::ObjectArchetype
		Vector3 position;
		Vector3 linearVelocity;
		Vector3 angularVelocity;
		Quaternion orientation;
	};
	static_assert(std::is_trivially_copyable_v<HaloObjectState>);

	// Owning game server -> each neighbouring server whose region its objects are
	// within the halo band of. Directed, never broadcast: a server three regions away
	// has no use for this and the traffic is per tick, not per event.
	//
	// Batched because the alternative is one packet per object per tick per neighbour.
	// entryCount says how many of entries[] are live, and GamePacket::size is set to
	// cover only those - the rest of the array is never put on the wire. That is why
	// the batch size can be generous without costing anything on a quiet border.
	//
	// mSenderTick is the sender's tick counter at the moment the state was sampled.
	// The receiver applies at mSenderTick + lookahead rather than on arrival, exactly
	// as handoff does, or network jitter would decide which tick a shadow moves on and
	// the run would stop being reproducible.
	struct HaloUpdatePacket : public GamePacket {
		// Sized so a full batch stays inside a typical 1400-byte MTU: 20 entries at
		// 60 bytes is 1200, plus the header. Larger batches would be fragmented by
		// ENet, which costs a retransmit of the whole thing if any fragment is lost.
		static constexpr int MAX_ENTRIES = 20;

		int senderServerID;
		int senderTick;
		int entryCount;
		HaloObjectState entries[MAX_ENTRIES];

		HaloUpdatePacket(int senderServerID, int senderTick);

		// Appends one object, and grows `size` to match. Returns false when the batch
		// is full, which is the caller's signal to send this packet and start another.
		bool TryAdd(const HaloObjectState& state);
	};
	static_assert(std::is_trivially_copyable_v<HaloUpdatePacket>);

	// One server's region, on the wire. Floats rather than the "minX/maxX|minZ/maxZ"
	// string the bootstrap packet uses: that format exists because the manager
	// serialises borders as text for the launch string, and re-parsing text here would
	// be a second place for a rounding difference to creep in between servers.
	struct RegionBoundsWire {
		int serverID;
		float minX;
		float maxX;
		float minZ;
		float maxZ;
	};
	static_assert(std::is_trivially_copyable_v<RegionBoundsWire>);

	// Manager -> every game server (and every client, which needs the same partition
	// to route commands). The partition becomes `regions` at tick `effectiveTick`.
	//
	// An ABSOLUTE tick, not an offset from receipt. Every server must switch on the
	// same simulated tick: if two disagree about where a border is, even for one tick,
	// OwningServerFor gives different answers on each and an object is either owned by
	// both of them or by neither. An offset from receipt would put the switch wherever
	// the packet happened to land.
	struct DistributedRepartitionPacket : public GamePacket {
		// PAGED, for the same reason the server registry is: a fixed bound here would
		// cap the number of servers an instance can have just as the bootstrap arrays
		// did. 16 regions is 320 bytes of payload, well inside an MTU.
		static constexpr int MAX_REGIONS_PER_PAGE = 16;

		long long effectiveTick;
		// Across ALL pages. A partition is only adopted once this many regions have
		// arrived: adopting a partial one would leave the server disagreeing with its
		// peers about where the borders are, which is the one thing a repartition must
		// never do.
		int totalRegionCount;
		int regionCount;
		RegionBoundsWire regions[MAX_REGIONS_PER_PAGE];

		DistributedRepartitionPacket(long long effectiveTick, int totalRegionCount);
		bool TryAddRegion(const RegionBoundsWire& region);
	};
	static_assert(std::is_trivially_copyable_v<DistributedRepartitionPacket>);

	// One server's entry in the instance registry: where it is, and what it owns.
	//
	// Borders as floats rather than the "minX/maxX|minZ/maxZ" text the old bootstrap
	// packet carried. That format cost 256 bytes per server and needed parsing on
	// arrival, which is a second place for a rounding difference to appear between
	// servers; 16 bytes of float is exact and free to read.
	struct ServerRegistryEntry {
		int serverID;
		int port;
		char ip[PACKET_IP_LENGTH];
		float minX;
		float maxX;
		float minZ;
		float maxZ;
	};
	static_assert(std::is_trivially_copyable_v<ServerRegistryEntry>);

	// Manager -> game servers. One PAGE of the instance's server registry.
	//
	// This exists to remove a hard architectural ceiling. The registry used to ride
	// inside StartDistributedGameServerPacket as five fixed 20-entry arrays - the
	// largest being char borders[20][256] - which capped an instance at 20 servers
	// and already made the bootstrap message about 6 KB. Widening the arrays does not
	// help: sized for 200 servers the same packet would be over 50 KB, sent to every
	// server, whether or not the instance is that large.
	//
	// Paging removes the cap entirely. A receiver accumulates entries until it holds
	// totalServerCount of them and only then builds its world, so a page arriving late
	// or out of order costs nothing. Sent reliably, because the registry is one-shot
	// bootstrap state with no retry: a server missing one page never starts.
	struct DistributedServerRegistryPacket : public GamePacket {
		// 16 entries is 640 bytes of payload, comfortably inside a typical 1400-byte
		// MTU, so a page is never fragmented by ENet.
		static constexpr int MAX_ENTRIES_PER_PAGE = 16;

		int gameInstanceID;
		// Across ALL pages. The receiver uses this to know when it has the full set.
		int totalServerCount;
		int entryCount;
		ServerRegistryEntry entries[MAX_ENTRIES_PER_PAGE];

		DistributedServerRegistryPacket(int gameInstanceID, int totalServerCount);
		bool TryAddEntry(const ServerRegistryEntry& entry);
	};
	static_assert(std::is_trivially_copyable_v<DistributedServerRegistryPacket>);

	// Client -> game server. The region of the world this client needs to be told
	// about, as a circle on the XZ plane.
	//
	// Without this a server sends every client a snapshot of every object it owns, one
	// packet per object, at the full-snapshot rate. That is O(world) per client and is
	// the reason the system can simulate a large world but not serve one: at 6,000
	// objects per server it is 60,000 packets a second to each client from each server.
	//
	// A circle rather than the server's own rectangular region, because interest
	// follows the VIEWER and has nothing to do with where the partition happens to put
	// its borders - a client near a border is interested in objects on both sides.
	//
	// radius == 0 means "send me everything", which is what a client that never
	// declares interest gets, so the old behaviour is still expressible.
	//
	// radius < 0 means "send me NOTHING". That is what one game server tells another:
	// servers exchange state through handoff and the halo band and register no handler
	// for snapshots at all, so every snapshot sent to a peer server was discarded on
	// arrival. With two servers and one client that was two thirds of all snapshot
	// traffic.
	struct DistributedClientInterestPacket : public GamePacket {
		int playerID;
		Vector3 centre;
		float radius;

		DistributedClientInterestPacket(int playerID, const Vector3& centre, float radius);
	};
	static_assert(std::is_trivially_copyable_v<DistributedClientInterestPacket>);

	// Game server -> manager. What this server cost over the last reporting interval.
	//
	// The cost measure is CONTACTS, not object count and not milliseconds.
	//
	// Not object count, because balancing that does not balance the work: the best
	// partition found by hand for the shuttle workload held 100 objects against 300
	// and still had near-equal wall clock, because its CONTACT counts were near-equal.
	// Contact cost scales with local density, which object count does not see.
	//
	// Not milliseconds, because a measured duration is not reproducible: two runs of
	// the same configuration would balance differently, and every determinism claim in
	// this system would become conditional on machine timing. Contacts per tick are a
	// deterministic function of the simulation.
	struct DistributedServerLoadReportPacket : public GamePacket {
		int serverID;
		int gameInstanceID;
		// The tick this report covers up to. The manager decides using reports for one
		// specific tick rather than "the latest", so the decision does not depend on
		// arrival order.
		long long tick;
		int ownedObjects;
		// Summed over the interval since the previous report.
		long long contacts;
		// This server's current X extent, so the manager can move a boundary without
		// having to remember the partition it last sent.
		float minX;
		float maxX;

		// Where the load IS inside this server's region, as a histogram along X.
		//
		// A single total is not enough to place a border. A cluster sitting entirely
		// inside one region looks identical whether it is at the left edge or the
		// right, so a manager working from totals can only guess which way to move -
		// and guessing wrong walks the border straight past the cluster. That is
		// exactly what happened: the policy moved a border until one server held all
		// 4,000 objects and the other held none, having simply swapped which server was
		// overloaded.
		//
		// Eight buckets is 32 bytes and locates a border to an eighth of a region,
		// which is finer than one round's damped step ever moves it.
		static constexpr int LOAD_BUCKETS = 8;
		int bucketContacts[LOAD_BUCKETS];

		DistributedServerLoadReportPacket(int serverID, int gameInstanceID, long long tick,
			int ownedObjects, long long contacts, float minX, float maxX,
			const int* buckets);
	};
	static_assert(std::is_trivially_copyable_v<DistributedServerLoadReportPacket>);

	struct ClientPacket : public GamePacket {
		int		lastID;
		char	buttonstates[8];
		Vector3 cameraPosition;

		ClientPacket() {
			type = Received_State;
			size = sizeof(ClientPacket) - sizeof(GamePacket);
			cameraPosition = Vector3(0, 0, 0);
		}
	};

	struct SyncPlayerListPacket : public GamePacket {
		int playerList[4];

		SyncPlayerListPacket(std::vector<int>& serverPlayers);
		void SyncPlayerList(std::vector<int>& clientPlayerList) const;
	};

	struct GameStartStatePacket : public GamePacket {
		bool isGameStarted = false;
		int gameInstanceId;

		// Fixed array, not std::string: this struct is memcpy'd onto the wire.
		char levelSeed[64];
		GameStartStatePacket(bool val, int gameInstanceId, const std::string& seed);
	};

	struct GameEndStatePacket : public GamePacket {
		bool isGameEnded = false;
		int winningPlayerId;

		GameEndStatePacket(bool val, int winningPlayerId);
	};

	struct ClientPlayerInputPacket : public GamePacket {
		int lastId;
		PlayerInputs playerInputs;
		float mouseXLook = 0.0f;
		int playerID;

		ClientPlayerInputPacket(int lastId, const PlayerInputs& playerInputs);
		ClientPlayerInputPacket(int lastId, int playerID, const PlayerInputs& playerInputs);
	};

	struct ClientUseItemPacket : public GamePacket {
		int objectID;
		int playerID;

		ClientUseItemPacket(int objectID, int playerID);
	};

	struct ClientSyncBuffPacket : public GamePacket {
		int playerID;
		int buffID;
		bool toApply;

		ClientSyncBuffPacket(int playerID, int buffID, bool toApply);
	};

	struct ClientSyncItemSlotUsagePacket : public GamePacket {
		int playerID;
		int firstItemUsage;
		int secondItemUsage;

		ClientSyncItemSlotUsagePacket(int playerID, int firstItemUsage, int secondItemUsage);
	};

	struct ClientSyncItemSlotPacket : public GamePacket {
		int playerID;
		int slotId;
		int equippedItem;
		int usageCount;

		ClientSyncItemSlotPacket(int playerID, int slotId, int equippedItem, int usageCount);
	};

	struct ClientSyncLocalActiveSusCausePacket : public GamePacket {
		int playerID;
		int activeLocalSusCauseID;
		bool toApply;

		ClientSyncLocalActiveSusCausePacket(int playerID, int activeLocalSusCauseID, bool toApply);
	};

	struct ClientSyncLocalSusChangePacket : public GamePacket {
		int playerID;
		int changedValue;

		ClientSyncLocalSusChangePacket(int playerID, int changedValue);
	};

	struct ClientSyncGlobalSusChangePacket : public GamePacket {
		int changedValue;

		ClientSyncGlobalSusChangePacket(int changedValue);
	};

	struct ClientSyncLocationActiveSusCausePacket : public GamePacket {
		int cantorPairedLocation;
		int activeLocationSusCauseID;
		bool toApply;

		ClientSyncLocationActiveSusCausePacket(int playerID, int activeLocationSusCauseID, bool toApply);
	};

	struct ClientSyncLocationSusChangePacket : public GamePacket {
		int cantorPairedLocation;
		int changedValue;

		ClientSyncLocationSusChangePacket(int cantorPairedLocation, int changedValue);
	};

	struct SyncInteractablePacket : public GamePacket {
		int networkObjId;
		bool isOpen;
		int interactableItemType;

		SyncInteractablePacket(int networkObjectId, bool isOpen, int interactableItemType);
	};

	struct SyncObjectStatePacket : public GamePacket {
		int networkObjId;
		int objectState;

		SyncObjectStatePacket(int networkObjId, int objectState);
	};

	struct AnnouncementSyncPacket : public GamePacket {
		int annType;
		float time;
		int playerNo;
		AnnouncementSyncPacket(int annType, float time, int playerNo);
	};

	struct ClientInitPacket : public GamePacket {
		std::string playerName;

		ClientInitPacket(const std::string& playerName);
	};

	struct SyncPlayerIdNameMapPacket : public GamePacket {
		int playerIds[4] = { -1 , -1, -1,-1 };
		std::string playerNames[4];

		SyncPlayerIdNameMapPacket(const std::map<int, std::string>& playerIdNameMap);
	};

	struct GuardSpotSoundPacket : public GamePacket {
		int playerId;

		GuardSpotSoundPacket(const int playerId);
	};

	struct DistributedClientConnectedToSystemPacket : public GamePacket {
		int distributedClientType;
		int gameInstanceID;

		DistributedClientConnectedToSystemPacket(int gameInstanceID, DistributedSystemClientType clientType);
	};

	struct DistributedClientGetGameInstanceDataPacket : public GamePacket {
		int peerID;
		bool isGameInstanceFound;
		int gameInstanceID;
		int playerNumber;
		int playerCount;
		int objectsPerPlayer;

		DistributedClientGetGameInstanceDataPacket(bool isGameInstanceFound, int gameInstanceID, int playerNumber);
		DistributedClientGetGameInstanceDataPacket();
	};

	struct DistributedPhysicsClientConnectedToManagerPacket : public GamePacket {
		int physicsServerID;
		int physicsPacketDistributorPort;
		int gameInstanceID;
		char ipAddress[PACKET_IP_LENGTH];

		DistributedPhysicsClientConnectedToManagerPacket(int port, int physicsServerID, int gameInstanceID, const std::string& ipAddress);
	};

	struct DistributedClientConnectToPhysicsServerPacket : public GamePacket {
		int physicsPacketDistributorPort;
		int physicsServerID;
		char ipAddress[PACKET_IP_LENGTH];
		// The server's region as "minX/maxX|minZ/maxZ" (same format the manager ships to
		// game servers in RunDistributedPhysicsServerInstancePacket). Lets the client draw
		// which server owns which slice of the world. Fixed char array so it survives the
		// raw memcpy the ENet wire path does on this struct.
		char borderStr[256];
		DistributedClientConnectToPhysicsServerPacket(int port, int physicsServerID, const std::string& ipAddress, const std::string& borderStr);
	};

	struct DistributedPhysicsServerAllClientsAreConnectedPacket : public GamePacket {
		int gameInstanceID;
		int gameServerID;
		bool isGameServerReady;

		DistributedPhysicsServerAllClientsAreConnectedPacket(int gameInstanceID, int gameServerID, bool isGameServerReady);
	};

	struct DistributedClientsGameServersAreReadyPacket : public GamePacket {
		//TODO(erendgrmnc: add ip and port information for clients to connect with additional required data)
		char ipAddresses[2][PACKET_IP_LENGTH];
		int ports[2];

		DistributedClientsGameServersAreReadyPacket();
	};

	struct StartDistributedGameServerPacket : public GamePacket {
		int serverManagerPort;
		int gameInstanceID;

		int currentServerCount;
		int totalServerCount;

		int clientsToConnect;
		int objectsPerPlayer;

		// Hard bound on every array below. The packet is POD with fixed-size arrays
		// (a variable-length payload cannot cross a memcpy'd wire), so exceeding this
		// is a buffer overflow rather than a truncation - the constructor clamps and
		// reports instead.
		static constexpr int MAX_SERVERS = 20;

		// Indexed by SERVER ID, alongside borders[]. Runs to totalServerCount.
		int serverIDs[20];
		int serverPorts[20];
		char borders[20][256];
		char createdServerIPs[20][PACKET_IP_LENGTH];

		// Indexed alongside serverPorts[] and createdServerIPs[], which are filled in
		// the order servers registered with the manager - NOT by server ID. Without
		// this, a receiver has no way to learn which server a given IP/port belongs
		// to, and code that assumed index == id mislabelled its peer links.
		int connectedServerIDs[20];

		StartDistributedGameServerPacket(int serverManagerPort, int gameInstanceID, int clientsToConnect, int objectsPerPlayer, std::vector<int> serverPorts,
			std::vector<std::string> serverIps,
			std::vector<int> connectedServerIds,
			const std::map<int, const std::string>& serverBorderMap);
	};

	struct StartSimulatingObjectPacket : public GamePacket {
		int objectID;
		int newOwnerServerID;
		int senderServerID;

		NetworkState lastFullState;

		//TODO(erendgrmnc): need to decide which physics property should be passed.


		//linear stuff
		Vector3 mLinearVelocity;
		Vector3 mForce;

		//angular stuff
		Vector3 mAngularVelocity;
		Vector3 mTorque;
		Vector3 mInverseInertia;
		Matrix3 mInverseInertiaTensor;

		// --- appended for player-controlled avatars (increment 7) ---
		//
		// The ONLY change to this packet's wire format in the whole interaction
		// design, and the fields are APPENDED so every offset above is unchanged.
		//
		// Continuous input is state, not an event (it is never sequenced and never
		// relayed), so without carrying it across a handoff a driven avatar would
		// stall for a tick or two on every border crossing until the client's next
		// axis update reached the new owner.
		int mControllerPlayerID;      // -1 when nothing is driving this object
		Vector3 mMoveAxis;            // last applied movement axis

		// --- appended for deterministic handoff application ---
		//
		// The tick on which the sender released the object. Handoff SENDS are already
		// deterministic (fixed dt + deterministic positions mean the border check
		// fires on the same tick every run); only the moment of APPLICATION varies,
		// because it depends on when the packet happens to arrive. Scheduling
		// application at senderTick + lookahead removes that last source of run-to-run
		// variation without any inter-server barrier.
		long long mSenderTick;

		// --- appended for region-local world state ---
		//
		// What the object IS, so a receiver that does not already hold it can build it
		// rather than rejecting the handoff. Under the pre-seed model every server
		// holds a deactivated twin of every object, so handoff is "reactivate in
		// place" and the receiver never needs to know the shape. Once a server holds
		// only its own region, an incoming object may be genuinely unknown, and
		// without this the handoff has no way to construct it.
		//
		// Appended, like the avatar and mSenderTick fields: every offset above is
		// unchanged, which matters because the roles deploy separately.
		int mArchetypeID;

		StartSimulatingObjectPacket(int objectID, int newServerID, int senderServerID, NetworkState lastFullState, PhysicsObject& physicsObj);

		// Needed so callers can declare an output parameter to fill. The packet is
		// POD on the wire, so a zeroed instance is safe.
		StartSimulatingObjectPacket();
	};

	struct StartSimulatingObjectReceivedPacket : public GamePacket {
		int objectID;
		int newOwnerServerID;

		StartSimulatingObjectReceivedPacket(int objectID, int newOwnerServerID);
	};

	struct RunDistributedPhysicsServerInstancePacket : public GamePacket {

		int serverID;
		int midwareID;
		int gameInstanceID;

		char borderStr[256];
		RunDistributedPhysicsServerInstancePacket(int serverID, int gameInstanceID, int midwareID, std::string borderStr);
	};

	struct PhysicsServerMiddlewareConnectedPacket : public GamePacket {
		char ipAddress[PACKET_IP_LENGTH];

		PhysicsServerMiddlewareConnectedPacket(const std::string& ipAddress);
	};

	struct PhysicsServerMiddlewareDataPacket : public GamePacket {
		int middlewareID;
		int peerID;

		PhysicsServerMiddlewareDataPacket(int peerID, int middlewareID);
	};

	class NetworkObject {
	public:
		NetworkObject(GameObject& o, int id);
		virtual ~NetworkObject();

		//Called by clients
		virtual bool ReadPacket(GamePacket& p);
		//Called by servers
		virtual bool WritePacket(GamePacket** p, bool deltaFrame, int stateID);
		virtual bool WritePacket(GamePacket** p, bool deltaFrame, int stateID, int gameServerID);

		GameObject& GetGameObject() { return object; }

		void SetGameObject(GameObject& obj) const { object = obj; }

		int GetNetworkID() { return networkID; }
		int GetNewServerID() const;

		void UpdateStateHistory(int minID);
		// Hard cap on stateHistory, independent of acknowledgement-driven pruning.
		void TrimStateHistory();

		NetworkState& GetLatestNetworkState();
		void SetLatestNetworkState(NetworkState& lastState);

		void FinishTransitionToNewServer(int newServerID);
		void HandleTransitionComplete();

		// Destroy wins over a not-yet-dispatched handoff (race W1). The tick order
		// runs the network pump before HandleObjectTransitions, so this is the common
		// case and it is cleanly winnable: a pure local state reset, no wire change
		// and no change to the handoff protocol itself.
		void CancelPendingTransition() {
			mIsActualPosOutServer = false;
			mIsWaitingHandshake = false;
			mNewServerID = -1;
		}

		bool IsPendingTransition() const {
			return mIsActualPosOutServer || mIsWaitingHandshake;
		}
		void OnTransitionHandshakeReceived();
		void AddReceivedObjectLastPacket(const NetworkState& state);

		bool GetIsActualPosOutOfServer() const;
		bool GetIsPredictionInfoSent() const;

	protected:

		bool GetNetworkState(int frameID, NetworkState& state);

		virtual bool ReadDeltaPacket(DeltaPacket& p);
		virtual bool ReadFullPacket(FullPacket& p);

		virtual bool WriteDeltaPacket(GamePacket** p, int stateID);
		virtual bool WriteFullPacket(GamePacket** p);

		virtual bool WriteFullPacket(GamePacket** p, int gameServerID);
		virtual bool WriteDeltaPacket(GamePacket** p, int stateID, int gameServerID);

		GameObject& object;

		NetworkState lastFullState;

		std::vector<NetworkState> stateHistory;

		int deltaErrors;
		int fullErrors;

		int networkID;

		bool mIsPredictionInfoSent = false;

		bool mIsActualPosOutServer = false;
		bool mIsWaitingHandshake = false;

		float mPassedTransitionTime = 0.f;

		int mNewServerID = -1;
	};
}
#endif