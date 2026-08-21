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

			// The expected peer count is not known until the manager's start packet
			// arrives, which can be AFTER the last peer has already connected. The
			// readiness test therefore has to be re-evaluated when the bound changes,
			// not only when a peer joins.
			void SetMaxClients(int maxClients) override;
			void RegisterOnAllClientsAreConnectedEvent(const std::function<void()>& callback);

			// Fired for every peer that connects, with its peer number, so the owner
			// can send that peer alone a manifest of the objects it currently owns.
			// Without this a late joiner only learns about an object when its next
			// snapshot arrives - which loses the archetype, and never conveys an
			// object sitting in the handoff ownership gap at the moment of joining.
			void RegisterOnPeerJoinedEvent(const std::function<void(int)>& callback);
			// Fired when a peer goes away. Needed because per-peer state kept OUTSIDE
			// this class - the manager's declared snapshot interest, keyed by peer
			// number - outlives the peer otherwise, and ENet reuses peer numbers. The
			// next occupant of a slot then inherits the previous one's declaration.
			void RegisterOnPeerLeftEvent(const std::function<void(int)>& callback);
		protected:
			std::vector<std::function<void()>> mOnAllClientsAreConnected;
			std::vector<std::function<void(int)>> mOnPeerJoined;
			std::vector<std::function<void(int)>> mOnPeerLeft;

			void TriggerOnAllClientsAreConnectedEvents() const;

			// Fires the all-connected event once, when the peer count first reaches
			// the expected total.
			void CheckAllClientsConnected();
			bool mAllClientsTriggered = false;
		};
	}
}
 