#include "DistributedPhysicsManagerServer.h"

#include "NetworkObject.h"
#include "enet/enet.h"

NCL::Networking::DistributedPhysicsManagerServer::DistributedPhysicsManagerServer(int onPort, int maxClients) : GameServer(onPort, maxClients) {
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
			std::string strIp(ipAddress);
			bool isGameServer = IsConnectedPeerAGameServer(ipAddress);
			std::cout << "Server: New client has connected" << '\n';
			AddPeer(peer + 1, isGameServer, strIp);
		}
		else if (type == ENetEventType::ENET_EVENT_TYPE_DISCONNECT) {
			std::cout << "Server: Client has disconnected" << '\n';
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

void NCL::Networking::DistributedPhysicsManagerServer::AddPeer(int peerNumber, bool isPhysicsServerClient, const std::string& ipAddress) {
	int emptyIndex = mClientMax;
	mPeerIpAddressMap.insert(std::make_pair(peerNumber, ipAddress));
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
	}
}

void Networking::DistributedPhysicsManagerServer::HandleConnectedPhysicsServer(int gameInstanceId, int peerNumber) {
	int connectedServerCount = mServerPeerRunningInstanceCount[gameInstanceId];
	mServerPeerRunningInstanceCount[gameInstanceId] = ++connectedServerCount;
}

void Networking::DistributedPhysicsManagerServer::AddGameInstance(GameInstance* gameInstance) {
	mGameInstances.push_back(gameInstance);
}

const std::string& Networking::DistributedPhysicsManagerServer::GetPeerIpAddressStr(int peerNum) {
	return mPeerIpAddressMap[peerNum];
}

std::string Networking::DistributedPhysicsManagerServer::GetIPAddress() {
	if (!netHandle) {
		return ""; // Or handle the error appropriately
	}

	char ipAddress[15];
	enet_address_get_host_ip(&netHandle->address, ipAddress, 15);
	return std::string(ipAddress);
}

GameInstance* Networking::DistributedPhysicsManagerServer::GetGameInstance(int gameInstanceID) {
	for (const auto& instance : mGameInstances) {
		if (instance->GetGameID() == gameInstanceID) {
			return instance;
		}
	}
	return nullptr;
}
