#include "SystemManager.h"

#include <iostream>

#include "DistributedPhysicsManagerServer.h"
#include "GameServer.h"
#include "NetworkBase.h"
#include "NetworkObject.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/DistributedPhysicsServerDto.h"
#include "ServerWorldManager.h"

namespace {
	constexpr int GAME_AREA_MIN_X = -150;
	constexpr int GAME_AREA_MAX_X = 150;

	constexpr int GAME_AREA_MIN_Z = -150;
	constexpr int GAME_AREA_MAX_Z = 150;
}

NCL::DistributedManager::SystemManager::SystemManager(int maxPhysicsServerCount, int maxClientCount) {
	mDistributedPhysicsManagerServer = nullptr;
	mMaxPhysicsServerCount = maxPhysicsServerCount;
	mMaxClientCount = maxClientCount;
	CalculatePhysicsServerBorders();
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
		auto* distributedPhysicsServerAllClientsAreConnectedPacket = static_cast<NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket*>(payload);
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

void DistributedManager::SystemManager::SendStartDataToPhysicsServer(int physicsServerID) const {
	std::vector<int> serverPorts;
	std::vector<std::string> serverIps;

	for (const auto& createdServer : mDistributedPhysicsServers) {
		serverIps.push_back(createdServer->ipAddress);
		serverPorts.push_back(createdServer->dataSenderPort);
	}

	StartDistributedGameServerPacket packet(1234, mMaxClientCount,serverPorts, serverIps, mPhysicsServerBorderStrMap);
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(packet);
}

void NCL::DistributedManager::SystemManager::HandleDistributedClientConnectedPacketReceived(NCL::CSC8503::DistributedClientConnectedToSystemPacket* packet) {
	//TODO(erendgrmcn): Implement logic to register connected Game Client.
}

void DistributedManager::SystemManager::HandleDistributedPhysicsClientConnectedPacketReceived(
	NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket* packet) const {
	std::cout << "Distributed Physics Server Info Packet Sent! Ip: 127.0.0.1 | port: " << packet->phyiscsPacketDistributerPort << std::endl;
	int portForClientsToConnect = packet->phyiscsPacketDistributerPort;
	mDistributedPhysicsServers.at(packet->physicsServerID)->dataSenderPort = portForClientsToConnect;


	std::cout << "Sending physics server data packet to server: " << packet->physicsServerID << "\n";
	SendStartDataToPhysicsServer(packet->physicsServerID);

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

		if (CheckIsGameStartable()) {
			std::cout << "Starting Game!\n";
				SendStartGameStatusPacket();
		}
	}

}

void DistributedManager::SystemManager::CalculatePhysicsServerBorders() {
	for (int i = 0; i < mMaxPhysicsServerCount; i++) {
		GameBorder& border = CalculateServerBorders(i);
		std::pair<int, GameBorder*> pair = std::make_pair(i, &border);
		AddServerBorderDataToMap(pair);
	}
	SetPhysicsServerBorderStrMap();
}

void DistributedManager::SystemManager::SetPhysicsServerBorderStrMap() {
	std::cout << "-------------- SERVER BORDERS ---------------\n";
	for (const auto& pair : mPhysicsServerBorderMap) {
		const std::string& borderStr = GetServerAreaString(pair.first);
		std::pair<int, const std::string> strPair = std::make_pair(pair.first, borderStr);
		std::cout << "Server(" << pair.first << "): " << pair.second->minX << "," << pair.second->maxX << "|" << pair.second->minZ << ", " << pair.second->maxZ << "\n";
		mPhysicsServerBorderStrMap.insert(strPair);
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

NCL::GameBorder& DistributedManager::SystemManager::CalculateServerBorders(int serverNum) {
	if (serverNum < 0 || serverNum > mMaxPhysicsServerCount) {
		throw std::out_of_range("Server number out of range");
	}

	GameBorder* border = new GameBorder(0.f, 0.f, 0.f, 0.f);

	// Calculate the number of columns and rows based on the number of servers
	int numCols, numRows;
	if (mMaxPhysicsServerCount == 3) {
		// Special case for 3 servers
		numCols = 2;  // 2 columns
		numRows = 2;  // 2 rows, but we only use 1 row in the bottom row
	}
	else {
		double sqrt = std::sqrt(mMaxPhysicsServerCount);
		numCols = static_cast<int>(std::ceil(sqrt));
		numRows = static_cast<int>(std::ceil(static_cast<double>(mMaxPhysicsServerCount) / numCols));
	}

	// Calculate the width and height of each region
	int rectWidth = (GAME_AREA_MAX_X - GAME_AREA_MIN_X) / numCols;
	int rectHeight = (GAME_AREA_MAX_Z - GAME_AREA_MIN_Z) / numRows;

	// Determine which row and column this server is responsible for
	int row = serverNum / numCols;
	int col = serverNum % numCols;

	// Set the borders
	border->minX = GAME_AREA_MIN_X + col * rectWidth;
	border->minZ = GAME_AREA_MIN_Z + row * rectHeight;
	border->maxX = (col == numCols - 1) ? GAME_AREA_MAX_X : (border->minX + rectWidth);
	border->maxZ = (row == numRows - 1) ? GAME_AREA_MAX_Z : (border->minZ + rectHeight);

	// Adjustments for the special 3-server layout
	if (mMaxPhysicsServerCount == 3) {
		if (serverNum == 2) {
			// The bottom row server spans the full width
			border->minX = GAME_AREA_MIN_X;
			border->maxX = GAME_AREA_MAX_X;
		}
	}

	return *border;
}

std::string DistributedManager::SystemManager::GetServerAreaString(int serverID) {
	std::stringstream ss;
	GameBorder* borders = mPhysicsServerBorderMap[serverID];
	ss << borders->minX << "/" << borders->maxX << "|" << borders->minZ << "/" << borders->maxZ;
	return ss.str();
}

void DistributedManager::SystemManager::AddServerData(DistributedPhysicsServerData& data) {
	mDistributedPhysicsServers.push_back(&data);
}

void DistributedManager::SystemManager::AddServerBorderDataToMap(
	std::pair<int, GameBorder*>& pair) {
	mPhysicsServerBorderMap.insert(pair);
}

NCL::Networking::DistributedPhysicsManagerServer* NCL::DistributedManager::SystemManager::GetServer() const {
	return mDistributedPhysicsManagerServer;
}

bool NCL::DistributedManager::SystemManager::GetIsServerRunning() const {
	return mDistributedPhysicsManagerServer != nullptr;
}
