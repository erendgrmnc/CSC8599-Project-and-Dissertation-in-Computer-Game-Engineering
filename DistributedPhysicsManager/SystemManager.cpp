#include "SystemManager.h"

#include <iostream>

#include "DistributedPhysicsManagerServer.h"
#include "GameServer.h"
#include "NetworkBase.h"
#include "NetworkObject.h"

NCL::DistributedManager::SystemManager::SystemManager(int maxPhysicsServerCount) {
	mDistributedPhysicsManagerServer = nullptr;
	mMaxPhysicsServerCount = maxPhysicsServerCount;

	NetworkBase::Initialise();
}

NCL::DistributedManager::SystemManager::~SystemManager() {
	delete mDistributedPhysicsManagerServer;
}

void NCL::DistributedManager::SystemManager::StartManagerServer(int port, int maxClients) {
	mDistributedPhysicsManagerServer = new NCL::Networking::DistributedPhysicsManagerServer(port, maxClients);

	std::cout << "Server started..." << "\n";

	RegisterPacketHandlers();
}

void NCL::DistributedManager::SystemManager::RegisterPacketHandlers() {
	mDistributedPhysicsManagerServer->RegisterPacketHandler(Received_State, this);
	mDistributedPhysicsManagerServer->RegisterPacketHandler(String_Message, this);
	mDistributedPhysicsManagerServer->RegisterPacketHandler(DistributedClientConnectedToManager, this);
	mDistributedPhysicsManagerServer->RegisterPacketHandler(DistributedPhysicsClientConnectedToManager, this);
	mDistributedPhysicsManagerServer->RegisterPacketHandler(BasicNetworkMessages::DistributedPhysicsServerAllClientsAreConnected, this);
}

void NCL::DistributedManager::SystemManager::ReceivePacket(int type, GamePacket* payload, int source) {
	switch (type) {
	case DistributedClientConnectedToManager: {
		auto* distributedClientConnectPacket = (NCL::CSC8503::DistributedClientConnectedToSystemPacket*)payload;
		HandleDistributedClientConnectedPacketReceived(distributedClientConnectPacket);
		break;
	}
	case DistributedPhysicsClientConnectedToManager: {
		auto* distributedPhysicsClientConnectedToManagerPacket = (NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket*)payload;
		HandleDistributedPhysicsClientConnectedPacketReceived(distributedPhysicsClientConnectedToManagerPacket);
		break;
	}
	case DistributedPhysicsServerAllClientsAreConnected: {
		auto* distributedPhysicsServerAllClientsAreConnectedPacket = (NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket*)payload;
		HandleAllClientsConnectedToPhysicsServer(distributedPhysicsServerAllClientsAreConnectedPacket);
		break;
	}
	default:
		std::cout << "Received unknown packet. Type: " << payload->type << std::endl;
		break;
	}
}

void NCL::DistributedManager::SystemManager::SendStartGameStatusPacket() {
	mIsGameStarted = true;
	GameStartStatePacket state(mIsGameStarted, "");
	mDistributedPhysicsManagerServer->SendGlobalPacket(state);
}

void DistributedManager::SystemManager::
SendDistributedPhysicsServerInfoToClients(const std::string& ip, const int serverID, const int port) const {
	DistributedClientConnectToPhysicsServerPacket packet(port, serverID, ip);
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(packet);
}

void NCL::DistributedManager::SystemManager::HandleDistributedClientConnectedPacketReceived(NCL::CSC8503::DistributedClientConnectedToSystemPacket* packet) {
	//TODO(erendgrmcn): Implement logic to register connected Game Client.
}

void DistributedManager::SystemManager::HandleDistributedPhysicsClientConnectedPacketReceived(
	NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket* packet) const {
	std::cout << "Distributed Physics Server Info Packet Sent! Ip: 127.0.0.1 | port: " << packet->phyiscsPacketDistributerPort << std::endl;
	int portForClientsToConnect = packet->phyiscsPacketDistributerPort;
	SendDistributedPhysicsServerInfoToClients("127.0.0.1", packet->physicsServerID, portForClientsToConnect);
}

void DistributedManager::SystemManager::HandleDistributedPhysicsServerAllClientsAreConnectedPacketReceived(
	NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet) {
	//TODO(erendgrmnc): Implement logic to register all game clients are connected to a specific distributed physics server.

}

void DistributedManager::SystemManager::HandleAllClientsConnectedToPhysicsServer(
	NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet) {
	if (packet->isGameServerReady) {
		for (const auto& server : mDistributedPhysicsServers) {
			if (server->serverId == packet->gameServerId) {
				server->isAllClientsConnectedToServer = true;
				break;
			}
		}

		//TODO IFte hata var.

		if (CheckIsGameStartable()) {
			std::cout << "Starting Game!\n";
			SendStartGameStatusPacket();
		}
	}

}

bool DistributedManager::SystemManager::CheckIsGameStartable() {
	if (mDistributedPhysicsServers.size() != mMaxPhysicsServerCount) {
		return false;
	}

	for (const auto& server : mDistributedPhysicsServers) {
		if (!server->isServerStarted || !server->isAllClientsConnectedToServer)
			return false;
	}

	return true;
}

void DistributedManager::SystemManager::AddServerData(DistributedPhysicsServerData& data) {
	mDistributedPhysicsServers.push_back(&data);
}

NCL::Networking::DistributedPhysicsManagerServer* NCL::DistributedManager::SystemManager::GetServer() const {
	return mDistributedPhysicsManagerServer;
}

bool NCL::DistributedManager::SystemManager::GetIsServerRunning() const {
	return mDistributedPhysicsManagerServer != nullptr;
}
