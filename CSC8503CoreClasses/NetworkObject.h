#include "DistributedSystemCommonFiles/DistributedPhysicsServerDto.h"
#ifdef USEGL
#pragma once
#include "GameObject.h"
#include "NetworkBase.h"
#include "NetworkState.h"
#include "../CSC8503/NetworkPlayer.h"

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

		SyncPlayerIdNameMapPacket(const std::map<int, string>& playerIdNameMap);
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

		DistributedClientGetGameInstanceDataPacket(bool isGameInstanceFound, int gameInstanceID, int playerNumber);
		DistributedClientGetGameInstanceDataPacket();
	};

	struct DistributedPhysicsClientConnectedToManagerPacket : public GamePacket {
		int physicsServerID;
		int phyiscsPacketDistributerPort;
		int gameInstanceID;

		DistributedPhysicsClientConnectedToManagerPacket(int port, int physicsServerID, int gameInstanceID);
	};

	struct DistributedClientConnectToPhysicsServerPacket : public GamePacket {
		int physicsPacketDistributerPort;
		int physicsServerID;
		std::string ipAddress;
		DistributedClientConnectToPhysicsServerPacket(int port, int physicsServerID, const std::string& ipAddress);
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

		int serverIDs[20];
		int serverPorts[20];
		std::string borders[20];
		std::string createdServerIPs[20];

		StartDistributedGameServerPacket(int serverManagerPort, int gameInstanceID, int clientsToConnect, std::vector<int> serverPorts,
			std::vector<std::string> serverIps,
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
		Matrix3 mInverseInteriaTensor;

		StartSimulatingObjectPacket(int objectID, int newServerID, int senderServerID, NetworkState lastFullState, PhysicsObject& physicsObj);
	};

	struct StartSimulatingObjectReceivedPacket : public GamePacket {
		int objectID;
		int newOwnerServerID;

		StartSimulatingObjectReceivedPacket(int objectID, int newOwnerServerID);
	};

	struct RunDistributedPhysicsServerInstancePacket : public GamePacket {
		std::string borderStr;
		int serverID;
		int midwareID;
		int gameInstanceID;

		RunDistributedPhysicsServerInstancePacket(int serverID, int gameInstanceID, int midwareID, const std::string& borderStr);
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

		int GetnetworkID() { return networkID; }
		int GetNewServerID() const;

		void UpdateStateHistory(int minID);

		NetworkState& GetLatestNetworkState();
		void SetLatestNetworkState(NetworkState& lastState);

		void FinishTransitionToNewServer(int newServerID);
		void HandleTransitionComplete();
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