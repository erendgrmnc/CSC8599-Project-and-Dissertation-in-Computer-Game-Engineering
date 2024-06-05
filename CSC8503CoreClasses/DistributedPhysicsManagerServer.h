#pragma once
#include "GameServer.h"
namespace NCL {
	namespace Networking {
		class DistributedPhysicsManagerServer : public NCL::CSC8503::GameServer {
		public:
			DistributedPhysicsManagerServer(int onPort, int maxClients);
			~DistributedPhysicsManagerServer();

			bool IsConnectedPeerAGameServer(std::string ipAddress);
			bool SendGlobalReliablePacket(GamePacket& packet) const;

			void UpdateServer() override;
			void AddPeer(int peerNumber, bool isPhysicsServerClient);
			void SendStartGameStatusPacket() const;
			void SendClientsGameServersAreConnectable() const;
		protected:
			bool mIsAllGameServersConnected = false;

			std::vector<int> mConnectedGameClientPeers;
			std::vector<int> mConnectedPhysicsClientPeers;

			std::vector<std::string> mDistributedPhysicsServerIps;
		};
	}
}
