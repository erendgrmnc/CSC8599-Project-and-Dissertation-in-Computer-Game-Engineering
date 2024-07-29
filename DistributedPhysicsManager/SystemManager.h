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

		struct GameBorder {
			double maxX = 0.f;
			double minX = 0.f;
			double	 minZ = 0.f;
			double maxZ = 0.f;

			GameBorder(double minX, double maxX, double minZ, double maxZ) {
				this->maxX = maxX;
				this->minX = minX;
				this->minZ = minZ;
				this->maxZ = maxZ;
			}
		};
		struct DistributedPhysicsServerData {
			bool isServerStarted = false;
			bool isAllClientsConnectedToServer = false;

			std::string ipAddress;
			int dataSenderPort;
			int serverId;
		};

		class SystemManager : public PacketReceiver {
		public:
			SystemManager(int maxPhysicsServerCount);
			~SystemManager();

			bool GetIsServerRunning() const;

			NCL::Networking::DistributedPhysicsManagerServer* GetServer() const;

			void StartManagerServer(int port, int maxClients);
			void RegisterPacketHandlers();
			void ReceivePacket(int type, GamePacket* payload, int source) override;
			void SendStartGameStatusPacket();
			void AddServerData(DistributedPhysicsServerData& data);
			void AddServerBorderDataToMap(std::pair<int, GameBorder*>& pair);

			std::string GetServerAreaString(int serverID);
		protected:
			bool mIsGameStarted = false;
			int mMaxPhysicsServerCount = 0;

			NCL::Networking::DistributedPhysicsManagerServer* mDistributedPhysicsManagerServer = nullptr;

			std::vector<DistributedPhysicsServerData*> mDistributedPhysicsServers;
			std::map<int, GameBorder*> mPhysicsServerBorderMap;
			std::map<int, const std::string> mPhysicsServerBorderStrMap;

			void SendDistributedPhysicsServerInfoToClients(const std::string& ip, const int serverID, const int port) const;
			void SendStartDataToPhysicsServer(int physicsServerID) const;

			void HandleDistributedClientConnectedPacketReceived(NCL::CSC8503::DistributedClientConnectedToSystemPacket* packet);
			void HandleDistributedPhysicsClientConnectedPacketReceived(NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket* packet) const;
			void HandleDistributedPhysicsServerAllClientsAreConnectedPacketReceived(NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet);
			void HandleAllClientsConnectedToPhysicsServer(NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet);

			void CalculatePhysicsServerBorders();
			void SetPhysicsServerBorderStrMap();
			bool CheckIsGameStartable();

			GameBorder& CalculateServerBorders(int serverNum);
		};

	}
}
