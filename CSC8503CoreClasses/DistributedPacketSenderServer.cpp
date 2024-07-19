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
			AddPeer(peer + 1);
		}
		else if (type == ENetEventType::ENET_EVENT_TYPE_DISCONNECT) {
			std::cout << "Server: Client has disconnected" << std::endl;
			for (int i = 0; i < 3; ++i) {
				if (mPeers[i] == peer + 1) {
					mPeers[i] = -1;
				}
			}
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
	std::cout << "Game client connected\n";
	if (mClientCount == mClientMax) {
		TriggerOnAllClientsAreConnectedEvents();
	}
}

void NCL::Networking::DistributedPacketSenderServer::RegisterOnAllClientsAreConnectedEvent(const std::function<void()>& callback) {
	mOnAllClientsAreConnected.push_back(callback);

}

void NCL::Networking::DistributedPacketSenderServer::TriggerOnAllClientsAreConnectedEvents() const {
	for (const auto& callback : mOnAllClientsAreConnected) {
		callback();
	}
}
