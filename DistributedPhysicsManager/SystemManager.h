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

			// --- dynamic repartitioning ---
			//
			// Broadcasts a new partition, to take effect at an ABSOLUTE tick. Sent to
			// game servers AND clients: a client routes commands with the same
			// OwningServerFor the servers use, so a client left on the old partition
			// would misroute every command issued near a moved border. It would still
			// be relayed to the right owner, but the relay hop is exactly what the
			// client-side routing exists to avoid.
			void SendRepartitionPacket(int gameInstanceID, long long effectiveTick,
				const std::vector<double>& interiorX);

			// Forced partition change for testing the mechanism before any policy
			// exists: --repartition-at TICK --repartition-x "x1,x2,...". 0 disables.
			void SetForcedRepartition(long long atTick, const std::vector<double>& interiorX) {
				mForcedRepartitionTick = atTick;
				mForcedRepartitionX = interiorX;
			}
			void AddServerData(DistributedPhysicsServerData& data);

			NCL::GameInstance* CreateNewGameInstance(int maxServer, int clientCount, int objectsPerPlayer,
				double worldMinX = -150.0, double worldMaxX = 150.0, double worldMinZ = -150.0, double worldMaxZ = 150.0);

			// Number of physics-server middlewares currently connected. The launcher's
			// --autostart path waits on this before creating the game instance so the
			// run-server packets have a recipient.
			int GetConnectedMidwareCount() const;
		protected:
			bool mIsGameStarted = false;
			int mMaxPhysicsServerCount = 0;
			int mMaxClientCount = 0;
			int mSystemManagerPort;

			NCL::Networking::DistributedPhysicsManagerServer* mDistributedPhysicsManagerServer = nullptr;

			std::vector<GameInstance*> mCreatedGameInstances;
			std::vector<DistributedPhysicsServerData*> mDistributedPhysicsServers;

			std::map<int, int> mPhysicsServerMiddlewareRunningInstanceMap;

			// Forced repartition, for testing the mechanism without a policy.
			long long mForcedRepartitionTick = 0;
			std::vector<double> mForcedRepartitionX;

			void SendDistributedPhysicsServerInfoToClients(const std::string& ip, const int serverID, const int port, const std::string& borderStr) const;
			void SendStartDataToPhysicsServer(int gameInstanceID, int physicsServerID) const;
			// The instance's server registry, in pages. Removes the 20-server cap the
			// fixed arrays in the bootstrap packet imposed.
			void SendServerRegistry(int gameInstanceID) const;
			void SendPhysicsServerMiddlewareDataPacket(int peerID, int midwareID);

			void HandleDistributedClientConnectedPacketReceived(int peerID, NCL::CSC8503::DistributedClientConnectedToSystemPacket* packet);
			void HandleDistributedPhysicsClientConnectedPacketReceived(int peerNumber, NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket* packet);
			void HandleDistributedPhysicsServerAllClientsAreConnectedPacketReceived(NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet);
			void HandleAllClientsConnectedToPhysicsServer(NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet);
			void HandlePhysicsServerMiddlewareConnected(int peerID, PhysicsServerMiddlewareConnectedPacket* packet);
			void SendRunServerInstancePacket(int gameInstance, int physicsServerID, int midwareID, std::string borderStr);

			void StartGameServers(int gameInstanceID);

			bool CheckIsGameStartable(int gameInstanceID);

			std::vector<DistributedPhysicsServerData*> GetPhysicsServerDataList(int gameInstanceID) const;

			int GetAvailablePhysicsMidware(); 

			GameBorder& CalculateServerBorders(int serverNum);
		};

	}
}
