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

			// Fired for every peer that connects, with its peer number, so the owner
			// can send that peer alone a manifest of the objects it currently owns.
			// Without this a late joiner only learns about an object when its next
			// snapshot arrives - which loses the archetype, and never conveys an
			// object sitting in the handoff ownership gap at the moment of joining.
			void RegisterOnPeerJoinedEvent(const std::function<void(int)>& callback);
		protected:
			std::vector<std::function<void()>> mOnAllClientsAreConnected;
			std::vector<std::function<void(int)>> mOnPeerJoined;

			void TriggerOnAllClientsAreConnectedEvents() const;
		};
	}
}
 