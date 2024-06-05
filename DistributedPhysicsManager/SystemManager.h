#pragma once
#include "DistributedGameServerManager.h"
#include "NetworkBase.h"

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

		struct DistributedPhysicsServerData {
			bool isServerStarted = false;
			bool isAllClientsConnectedToServer = false;
			std::string ipAddress;
			int serverId;
		};

		class SystemManager : public PacketReceiver {
		public:
			SystemManager();
			~SystemManager();

			bool GetIsServerRunning() const;

			NCL::Networking::DistributedPhysicsManagerServer* GetServer() const;

			void StartManagerServer(int port, int maxClients);
			void RegisterPacketHandlers();
			void ReceivePacket(int type, GamePacket* payload, int source) override;
			void SendStartGameStatusPacket();
			void AddServerData(DistributedPhysicsServerData& data);
		protected:
			bool mIsGameStarted = false;

			NCL::Networking::DistributedPhysicsManagerServer* mDistributedPhysicsManagerServer = nullptr;

			std::vector<DistributedPhysicsServerData*> mDistributedPhysicsServers;

			void SendDistributedPhysicsServerInfoToClients(const std::string& ip, const int port) const;

			void HandleDistributedClientConnectedPacketReceived(NCL::CSC8503::DistributedClientConnectedToSystemPacket* packet);
			void HandleDistributedPhysicsClientConnectedPacketReceived(NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket* packet) const;
			void HandleDistributedPhysicsServerAllClientsAreConnectedPacketReceived(NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet);
			void HandleAllClientsConnectedToPhysicsServer(NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet);
			bool CheckIsGameStartable();
		};

	}
}
