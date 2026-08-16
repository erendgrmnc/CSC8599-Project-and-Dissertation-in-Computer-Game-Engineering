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

		std::string levelSeed;
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
		std::string ipAddress;

		DistributedPhysicsClientConnectedToManagerPacket(int port, int physicsServerID, int gameInstanceID, std::string ipAddress);
	};

	struct DistributedClientConnectToPhysicsServerPacket : public GamePacket {
		int physicsPacketDistributorPort;
		int physicsServerID;
		std::string ipAddress;
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
		std::string ipAddresses[2];
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
		std::string createdServerIPs[20];

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

		StartSimulatingObjectPacket(int objectID, int newServerID, int senderServerID, NetworkState lastFullState, PhysicsObject& physicsObj);
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
		std::string ipAddress;

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