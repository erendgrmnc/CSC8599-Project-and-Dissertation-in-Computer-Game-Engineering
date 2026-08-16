#include "DistributedPacketSenderServer.h"

#include "enet/enet.h"

NCL::Networking::DistributedPacketSenderServer::DistributedPacketSenderServer(int onPort, int maxClients) : GameServer(onPort, maxClients){
	std::cout << "Starting packet sender server on port: " << mPort << '\n';
}

NCL::Networking::DistributedPacketSenderServer::~DistributedPacketSenderServer(){
	GameServer::~GameServer();
}

void NCL::Networking::DistributedPacketSenderServer::UpdateServer() {
	if (!netHandle) { return; }

	ENetEvent event;
	while (enet_host_service(netHandle, &event, 0) > 0) {
		int type = event.type;
		ENetPeer* p = event.peer;
		int peer = p->incomingPeerID;

		if (type == ENetEventType::ENET_EVENT_TYPE_CONNECT) {
			std::cout << "Server: New client has connected" << std::endl;
			// Retain the handle BEFORE AddPeer: AddPeer fires the peer-joined event,
			// whose handler sends this peer a directed manifest. Without the handle
			// stored first that send silently finds no destination - which is exactly
			// what happened, because this override duplicates GameServer's event loop
			// and did not store it.
			mPeerHandles[peer + 1] = p;
			AddPeer(peer + 1);
		}
		else if (type == ENetEventType::ENET_EVENT_TYPE_DISCONNECT) {
			std::cout << "Server: Client has disconnected" << std::endl;
			// Was a hardcoded i < 3 - a second copy of the bug already fixed in
			// GameServer::UpdateServer. Peers in any slot past index 2 were never
			// released, and mClientCount was never decremented at all, so this
			// server's peer table filled up permanently over a long run.
			for (int i = 0; i < mClientMax; ++i) {
				if (mPeers[i] == peer + 1) {
					mPeers[i] = -1;
					mClientCount--;
				}
			}
			mPeerHandles.erase(peer + 1);
		}
		else if (type == ENetEventType::ENET_EVENT_TYPE_RECEIVE) {
			//std::cout << "Server: Has recieved packet" << std::endl;
			GamePacket* packet = (GamePacket*)event.packet->data;
			ProcessPacket(packet, peer);
		}
		enet_packet_destroy(event.packet);
	}
}

void NCL::Networking::DistributedPacketSenderServer::AddPeer(int peerNumber) {
	GameServer::AddPeer(peerNumber);
	std::cout << "Client connected to packet server! Client Count: "<< mClientCount <<"/" << mClientMax << "\n";

	// Fired per peer, before the all-connected event: a late joiner needs its
	// manifest whether or not it happens to be the one that completes the set.
	for (const auto& callback : mOnPeerJoined) {
		callback(peerNumber);
	}

	if (mClientCount == mClientMax) {
		TriggerOnAllClientsAreConnectedEvents();
	}
}

void NCL::Networking::DistributedPacketSenderServer::RegisterOnPeerJoinedEvent(
	const std::function<void(int)>& callback) {
	mOnPeerJoined.push_back(callback);
}

void NCL::Networking::DistributedPacketSenderServer::RegisterOnAllClientsAreConnectedEvent(const std::function<void()>& callback) {
	mOnAllClientsAreConnected.push_back(callback);

}

void NCL::Networking::DistributedPacketSenderServer::TriggerOnAllClientsAreConnectedEvents() const {
	for (const auto& callback : mOnAllClientsAreConnected) {
		callback();
	}
}
