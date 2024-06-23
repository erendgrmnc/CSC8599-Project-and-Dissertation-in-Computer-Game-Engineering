#pragma once
#include <mutex>
#include <queue>

#include "DistributedPhysicsServerClient.h"
#include "NetworkBase.h"

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
	class ServerWorldManager;
}

namespace NCL {
	namespace DistributedGameServer {
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
			void ReceivePacket(int type, GamePacket* payload, int source) override;
			void BroadcastSnapshot(bool deltaFrame);
			void SendPacketsThread();
			void HandleGameStarted(CSC8503::GameStartStatePacket* gameStartPacket);
			void StartGame();
			void SendAllClientsAreConnectedToPacketSenderServerPacket() const;
			void SendPacketSenderServerStartedPacket(int port) const;
		protected:
			bool mIsServerConnectedToManager = false;
			bool mIsGameStarted = false;

			int mGameServerId;
			int mServerSideLastFullID;
			int mPacketsToSnapshot;

			int mPacketSenderServerPort;
			int mMaxGameClientsToConnectPacketSender;

			float mTimeToNextPacket;

			std::queue<GamePacket*> mPacketToSendQueue;

			std::mutex mPacketToSendQueueMutex;

			std::vector<CSC8503::NetworkObject*> mNetworkObjects;

			NCL::Networking::DistributedPhysicsServerClient* mThisDistributedPhysicsServer = nullptr;
			NCL::Networking::DistributedPacketSenderServer* mDistributedPacketSenderServer = nullptr;

			NCL::DistributedGameServer::ServerWorldManager* mServerWorldManager;
		};
	}
}

