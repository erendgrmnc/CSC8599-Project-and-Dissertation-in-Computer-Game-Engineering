#pragma once
#include <mutex>
#include <queue>

#include "DistributedPhysicsServerClient.h"
#include "NetworkBase.h"
#include "NetworkObject.h"

namespace NCL::CSC8503 {
	struct DistributedManagerAllGameServersAreConnectedPacket;
	struct GameStartStatePacket;
}

namespace NCL::Networking {
	class DistributedPacketSenderServer;
}

namespace NCL::CSC8503 {
	class NetworkObject;
}

namespace NCL::DistributedGameServer {
	struct PhyscisServerBorderData;
	class ServerWorldManager;
}


namespace NCL {
	namespace DistributedGameServer {
		struct GameServerConnection {
			int serverID;
			GameClient* client = nullptr;

			GameServerConnection(int serverID, GameClient* client);
		};

		class DistributedGameServerManager : public PacketReceiver {
		public:
			DistributedGameServerManager(int serverId, const std::string& serverBordersStr);
			~DistributedGameServerManager();

			NCL::Networking::DistributedPhysicsServerClient* GetDistributedPhysicsServer() const;
			NCL::DistributedGameServer::ServerWorldManager* GetServerWorldManager() const;

			bool StartDistributedGameServer(char a, char b, char c, char d, int port);
			bool StartDistributedPacketSenderServer();
			bool GetGameStarted() const;

			void UpdateGameServerManager(float dt);
			void RegisterGameServerPackets();
			void RegisterPacketSenderServerPackets();
			void UpdateMinimumState();
			void HandleClientPlayerInputPacket(ClientPlayerInputPacket* packet, int playerPeerID);
			void ReceivePacket(int type, GamePacket* payload, int source) override;
			void BroadcastSnapshot(bool deltaFrame);
			void SendPacketsThread();
			void HandleGameStarted(CSC8503::GameStartStatePacket* gameStartPacket);
			void StartGame();
			void SendAllClientsAreConnectedToPacketSenderServerPacket() const;
			void SendPacketSenderServerStartedPacket(int port) const;
		
			void HandleTransitionHandshakePacketReceived(StartSimulatingObjectReceivedPacket* packet);

		protected:
			bool mIsServerConnectedToManager = false;
			bool mIsGameStarted = false;
			bool mIsPlayerObjectsCreated = false;

			int mGameServerId;
			int mServerSideLastFullID;
			int mPacketsToSnapshot;

			int mPacketSenderServerPort;
			int mMaxGameClientsToConnectPacketSender;

			float mTimeToNextPacket;
			float mDebugTimer = 5.f;

			std::queue<GamePacket*> mPacketToSendQueue;

			std::mutex mPacketToSendQueueMutex;

			std::vector<CSC8503::NetworkObject*>* mNetworkObjects;

			std::map<int, int> mStateIDs;
			std::map<const int, PhyscisServerBorderData*> mPhysicsServerBorderMap;

			NCL::Networking::DistributedPhysicsServerClient* mThisDistributedPhysicsServer = nullptr;
			NCL::Networking::DistributedPacketSenderServer* mDistributedPacketSenderServer = nullptr;
			std::vector<GameServerConnection*> mDistributedPhysicsClients;

			NCL::DistributedGameServer::ServerWorldManager* mServerWorldManager;

			std::vector<char> IpToCharArray(const std::string& ipAddress);

			PhyscisServerBorderData* CreatePhysicsServerBorders(const std::string& borderString);

			void HandleStartGameServerPacketReceived(StartDistributedGameServerPacket* packet);
			void HandleObjectTransitions() const;
			void SendFinishTransactionPacket(NetworkObject& obj) const;
			void SendTransactionHandshakePacket(int senderServerID, int networkID) const;
;			GameServerConnection* ConnectServerToAnotherGameServer(char a, char b, char c, char d, int port, int gameServerID);
		};
	}
}

