#pragma once
#include "DistributedGameServerManager.h"
#include "NetworkBase.h"
#include "DistributedSystemCommonFiles/DistributedPhysicsServerDto.h"

namespace NCL::CSC8503 {
	struct DistributedPhysicsServerAllClientsAreConnectedPacket;
	struct DistributedPhysicsClientConnectedToManagerPacket;
}

namespace NCL::Networking {
	class DistributedPhysicsManagerServer;
}

namespace NCL::CSC8503 {
	struct DistributedClientConnectedToSystemPacket;
}

namespace NCL::CSC8503 {
	class GameServer;
}

namespace NCL {
	namespace DistributedManager {
		class SystemManager : public PacketReceiver {
		public:
			SystemManager(int maxPhysicsServerCount, int maxClientCount);
			~SystemManager();

			bool GetIsServerRunning() const;

			NCL::Networking::DistributedPhysicsManagerServer* GetServer() const;

			void StartManagerServer(int port, int maxClients);
			void RegisterPacketHandlers();
			void ReceivePacket(int type, GamePacket* payload, int source) override;
			void SendStartGameStatusPacket(int gameInstanceID);
			void AddServerData(DistributedPhysicsServerData& data);

			NCL::GameInstance* CreateNewGameInstance(int maxServer, int clientCount);
		protected:
			bool mIsGameStarted = false;
			int mMaxPhysicsServerCount = 0;
			int mMaxClientCount = 0;

			NCL::Networking::DistributedPhysicsManagerServer* mDistributedPhysicsManagerServer = nullptr;

			std::vector<GameInstance*> mCreatedGameInstances;
			std::vector<DistributedPhysicsServerData*> mDistributedPhysicsServers;
	

			void SendDistributedPhysicsServerInfoToClients(const std::string& ip, const int serverID, const int port) const;
			void SendStartDataToPhysicsServer(int gameInstanceID, int physicsServerID) const;
			void SendDistributedClientGameInstanceDataPacket();

			void HandleDistributedClientConnectedPacketReceived(int peerID, NCL::CSC8503::DistributedClientConnectedToSystemPacket* packet);
			void HandleDistributedPhysicsClientConnectedPacketReceived(int peerNumber, NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket* packet);
			void HandleDistributedPhysicsServerAllClientsAreConnectedPacketReceived(NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet);
			void HandleAllClientsConnectedToPhysicsServer(NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet);
			void SendRunServerInstancePacket(int gameInstance, int physicsServerID, const string& borderStr);

			void StartGameServers(int gameInstanceID);

			bool CheckIsGameStartable(int gameInstanceID);

			std::vector<DistributedPhysicsServerData*>& GetPhysicsServerDataList(int gameInstanceID) const;

			GameBorder& CalculateServerBorders(int serverNum);
		};

	}
}
