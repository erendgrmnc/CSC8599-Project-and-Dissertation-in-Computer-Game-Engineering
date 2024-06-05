#pragma once
#include "GameServer.h"

namespace NCL {
	namespace Networking {
		class DistributedPacketSenderServer : public CSC8503::GameServer {
		public:
			DistributedPacketSenderServer(int onPort, int maxClients);
			~DistributedPacketSenderServer();

			void UpdateServer() override;
			void AddPeer(int peerNumber) override;
			void RegisterOnAllClientsAreConnectedEvent(const std::function<void()>& callback);
		protected:
			std::vector<std::function<void()>> mOnAllClientsAreConnected;

			void TriggerOnAllClientsAreConnectedEvents() const;
		};
	}
}
 