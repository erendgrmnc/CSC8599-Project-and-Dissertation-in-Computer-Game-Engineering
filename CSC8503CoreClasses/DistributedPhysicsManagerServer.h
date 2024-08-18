#pragma once
#include "GameServer.h"

namespace NCL {
	struct GameInstance;
}

namespace NCL {
	namespace Networking {
		class DistributedPhysicsManagerServer : public NCL::CSC8503::GameServer {
		public:
			DistributedPhysicsManagerServer(int onPort, int maxClients);
			~DistributedPhysicsManagerServer();

			bool IsConnectedPeerAGameServer(std::string ipAddress);
			bool SendGlobalReliablePacket(GamePacket& packet) const;

			void UpdateServer() override;
			void AddPeer(int peerNumber, bool isPhysicsServerClient, const std::string& ipAddress);
			void HandleConnectedPhysicsServer(int gameInstanceId, int peerNumber);
			void AddGameInstance(GameInstance* gameInstance);

			const std::string& GetPeerIpAddressStr(int peerNum);
			std::string GetIPAddress();

			GameInstance* GetGameInstance(int gameInstanceID);
;		protected:
			std::vector<std::string> mDistributedPhysicsServerIps;

			std::vector<GameInstance*> mGameInstances;
			std::map<int, int> mServerPeerRunningInstanceCount;
			std::map<int, std::string> mPeerIpAddressMap;
		};
	}
}
