#include "DistributedPhysicsManagerServer.h"

#include "NetworkObject.h"
#include "enet/enet.h"

NCL::Networking::DistributedPhysicsManagerServer::DistributedPhysicsManagerServer(int onPort, int maxClients) : GameServer(onPort, maxClients) {
	//FOR TEST
	mDistributedPhysicsServerIps.push_back("127.0.0.1");
	mDistributedPhysicsServerIps.push_back("127.0.0.1");
}

NCL::Networking::DistributedPhysicsManagerServer::~DistributedPhysicsManagerServer() {

}

bool NCL::Networking::DistributedPhysicsManagerServer::IsConnectedPeerAGameServer(std::string ipAddress) {
	for (const auto& serverIp : mDistributedPhysicsServerIps) {
		if (ipAddress == serverIp) {
			return true;
		}
	}
	return false;
}

bool NCL::Networking::DistributedPhysicsManagerServer::SendGlobalReliablePacket(GamePacket& packet) const {
	ENetPacket* dataPacket = enet_packet_create(&packet, packet.GetTotalSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_host_broadcast(netHandle, 0, dataPacket);
	return true;
}

void NCL::Networking::DistributedPhysicsManagerServer::UpdateServer() {
	if (!netHandle) { return; }

	ENetEvent event;
	while (enet_host_service(netHandle, &event, 0) > 0) {
		int type = event.type;
		ENetPeer* p = event.peer;
		int peer = p->incomingPeerID;

		if (type == ENetEventType::ENET_EVENT_TYPE_CONNECT) {
			char ipAddress[15];
			enet_address_get_host_ip(&p->address, ipAddress, 15);
			bool isGameServer = IsConnectedPeerAGameServer(ipAddress);
			std::cout << "Server: New client has connected" << std::endl;
			AddPeer(peer + 1, isGameServer);
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

void NCL::Networking::DistributedPhysicsManagerServer::AddPeer(int peerNumber, bool isPhysicsServerClient) {
	int emptyIndex = mClientMax;
	for (int i = 0; i < mClientMax; i++) {
		if (mPeers[i] == peerNumber) {
			return;
		}
		if (mPeers[i] == -1) {
			emptyIndex = std::min(i, emptyIndex);
		}
	}
	if (emptyIndex < mClientMax) {
		mPeers[emptyIndex] = peerNumber;
		if (isPhysicsServerClient) {
			mConnectedPhysicsClientPeers.push_back(peerNumber);
			if (mConnectedPhysicsClientPeers.size() == mDistributedPhysicsServerIps.size()) {
				mIsAllGameServersConnected = true;

			}
		}
		else {
			mConnectedGameClientPeers.push_back(peerNumber);
		}
	}
}

void NCL::Networking::DistributedPhysicsManagerServer::SendStartGameStatusPacket() const {
	GameStartStatePacket state(true, "");
	SendGlobalReliablePacket(state);
}

void Networking::DistributedPhysicsManagerServer::SendClientsGameServersAreConnectable() const {
	DistributedClientsGameServersAreReadyPacket packet;
	packet.ipAddresses[0] = "127.0.0.1";
	packet.ipAddresses[1] = "127.0.0.1";

	packet.ports[0] = 1234;
	packet.ports[1] = 5678;

	SendGlobalReliablePacket(packet);
}
