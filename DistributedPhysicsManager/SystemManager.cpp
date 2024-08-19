#include "SystemManager.h"

#include <iostream>

#include "DistributedPhysicsManagerServer.h"
#include "GameServer.h"
#include "NetworkBase.h"
#include "NetworkObject.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/DistributedPhysicsServerDto.h"
#include "ServerWorldManager.h"
#include "DistributedSystemCommonFiles/DistributedUtils.h"

namespace {
	int GAME_INSTANCE_ID_BUFFER = 0;
	int PHYSICS_SERVER_ID_BUFFER = 0;
	int PHYSICS_MIDDLEWARE_ID_BUFFER = 0;
}

NCL::DistributedManager::SystemManager::SystemManager(int maxPhysicsServerCount, int maxClientCount) {
	mDistributedPhysicsManagerServer = nullptr;
	mMaxPhysicsServerCount = maxPhysicsServerCount;
	mMaxClientCount = maxClientCount;
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
	mDistributedPhysicsManagerServer->RegisterPacketHandler(BasicNetworkMessages::PhysicsServerMiddlewareConnected, this);
}

void NCL::DistributedManager::SystemManager::ReceivePacket(int type, GamePacket* payload, int source) {
	std::cout << "Packet Received, Type: " << type << "\n";
	switch (type) {
	case DistributedClientConnectedToManager: {
		auto* distributedClientConnectPacket = static_cast<NCL::CSC8503::DistributedClientConnectedToSystemPacket*>(payload);
		HandleDistributedClientConnectedPacketReceived(source, distributedClientConnectPacket);
		break;
	}
	case DistributedPhysicsClientConnectedToManager: {
		auto* distributedPhysicsClientConnectedToManagerPacket = (NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket*)payload;
		HandleDistributedPhysicsClientConnectedPacketReceived(source + 1, distributedPhysicsClientConnectedToManagerPacket);
		break;
	}
	case DistributedPhysicsServerAllClientsAreConnected: {
		auto* distributedPhysicsServerAllClientsAreConnectedPacket = static_cast<NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket*>(payload);
		HandleAllClientsConnectedToPhysicsServer(distributedPhysicsServerAllClientsAreConnectedPacket);
		break;
	}
	case BasicNetworkMessages::PhysicsServerMiddlewareConnected: {
		auto* physicsServerMiddlewareConnectedPacket = static_cast<NCL::CSC8503::PhysicsServerMiddlewareConnectedPacket*>(payload);
		HandlePhysicsServerMiddlewareConnected(source, physicsServerMiddlewareConnectedPacket);
		break;
	}
	default:
		std::cout << "Received unknown packet. Type: " << payload->type << std::endl;
		break;
	}
}

void NCL::DistributedManager::SystemManager::SendStartGameStatusPacket(int gameInstanceID) {
	mIsGameStarted = true;
	GameStartStatePacket state(mIsGameStarted, gameInstanceID, "");
	mDistributedPhysicsManagerServer->SendGlobalPacket(state);
}

void DistributedManager::SystemManager::
SendDistributedPhysicsServerInfoToClients(const std::string& ip, const int serverID, const int port) const {
	DistributedClientConnectToPhysicsServerPacket packet(port, serverID, ip);
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(packet);
}

void DistributedManager::SystemManager::SendStartDataToPhysicsServer(int gameInstanceID, int physicsServerID) const {
	std::vector<int> serverPorts;
	std::vector<std::string> serverIps;

	auto* gameInstance = mDistributedPhysicsManagerServer->GetGameInstance(gameInstanceID);

	for (const auto& createdServer : mDistributedPhysicsServers) {
		serverIps.push_back(createdServer->GetServerIPAddress());
		serverPorts.push_back(createdServer->GetDataSenderPort());
	}
	auto& physicsServersBorderStrMap = gameInstance->GetServerBorderStrMap();
	StartDistributedGameServerPacket packet(1234, gameInstanceID, mMaxClientCount, serverPorts, serverIps, physicsServersBorderStrMap);
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(packet);
}

void DistributedManager::SystemManager::SendPhysicsServerMiddlewareDataPacket(int peerID, int midwareID) {
	PhysicsServerMiddlewareDataPacket packet(peerID, midwareID);
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(packet);
}

void NCL::DistributedManager::SystemManager::HandleDistributedClientConnectedPacketReceived(int peerID, NCL::CSC8503::DistributedClientConnectedToSystemPacket* packet) {
	switch (packet->distributedClientType) {
	case DistributedSystemClientType::DistributedGameClient: {
		DistributedClientGetGameInstanceDataPacket dataPacket;
		dataPacket.peerID = peerID;
		GameInstance* gameInstance = mDistributedPhysicsManagerServer->GetGameInstance(packet->gameInstanceID);
		if (gameInstance == nullptr) {
			dataPacket.isGameInstanceFound = false;
		}
		else {
			dataPacket.gameInstanceID = gameInstance->GetGameID();
			dataPacket.playerNumber = gameInstance->AddPlayer(peerID);
			dataPacket.isGameInstanceFound = true;

			mDistributedPhysicsManagerServer->SendGlobalReliablePacket(dataPacket);

			if (gameInstance->IsServersReadyToStart()) {
				StartGameServers(gameInstance->GetGameID());
			}
		}
	}
	}
}


void DistributedManager::SystemManager::HandleDistributedPhysicsClientConnectedPacketReceived(int peerNumber,
	NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket* packet) {

	const std::string& ipAddress = mDistributedPhysicsManagerServer->GetPeerIpAddressStr(peerNumber);
	auto* serverData = DistributedUtils::CreatePhysicsServerData(ipAddress, packet->physicsServerID, packet->gameInstanceID);
	AddServerData(*serverData);

	std::string serverIpAddress = mDistributedPhysicsManagerServer->GetIPAddress();
	std::cout << "Distributed Physics Server Info Packet Sent! IP: " << serverIpAddress << "| port: " << packet->phyiscsPacketDistributerPort << std::endl;
	int portForClientsToConnect = packet->phyiscsPacketDistributerPort;
	serverData->SetDataSenderPort(portForClientsToConnect);


	std::cout << "Sending physics server data packet to server: " << packet->physicsServerID << "\n";
	SendStartDataToPhysicsServer(packet->gameInstanceID, packet->physicsServerID);

	SendDistributedPhysicsServerInfoToClients(serverData->GetServerIPAddress(), packet->physicsServerID, portForClientsToConnect);
}

void DistributedManager::SystemManager::HandleDistributedPhysicsServerAllClientsAreConnectedPacketReceived(
	NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet) {
	//TODO(erendgrmnc): Implement logic to register all game clients are connected to a specific distributed physics server.
}

void DistributedManager::SystemManager::HandleAllClientsConnectedToPhysicsServer(
	NCL::CSC8503::DistributedPhysicsServerAllClientsAreConnectedPacket* packet) {
	if (packet->isGameServerReady) {
		for (const auto& server : mDistributedPhysicsServers) {
			if (server->GetServerID() == packet->gameServerID) {
				server->SetIsAllClientsConnectedToServer(true);
				break;
			}
		}

		if (CheckIsGameStartable(packet->gameInstanceID)) {
			std::cout << "Starting Game!\n";
			SendStartGameStatusPacket(packet->gameInstanceID);
		}
	}
}

void DistributedManager::SystemManager::HandlePhysicsServerMiddlewareConnected(int peerID,
	PhysicsServerMiddlewareConnectedPacket* packet) {
	int newMidwareID = PHYSICS_MIDDLEWARE_ID_BUFFER++;
	std::pair<int, int> newPair = std::make_pair(newMidwareID, 0);
	std::cout << "Adding physics middleware with ID: " << newMidwareID << "\n";
	mPhysicsServerMiddlwareRunningInstanceMap.insert(newPair);

	SendPhysicsServerMiddlewareDataPacket(peerID, newMidwareID);
}

void DistributedManager::SystemManager::SendRunServerInstancePacket(int gameInstance, int physicsServerID,int midwareID, const std::string& borderStr) {
	RunDistributedPhysicsServerInstancePacket packet(physicsServerID, gameInstance, midwareID, borderStr);
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(packet);
}

void DistributedManager::SystemManager::StartGameServers(int gameInstanceID) {
	auto* gameInstance = mDistributedPhysicsManagerServer->GetGameInstance(gameInstanceID);
	int maxServer = gameInstance->GetServerCount();

	for (int i = PHYSICS_SERVER_ID_BUFFER; i < PHYSICS_SERVER_ID_BUFFER + maxServer; i++) {

		std::cout << "Creating server(" << i << ") for game instance: " << gameInstance->GetGameID() << "\n";
		const std::string& serverBorderStr = gameInstance->GetServerBorderStrMap()[i];
		int midwareID = GetAvailablePhysicsMidware();
		std::cout << "Sending create server to midware with ID: " << midwareID << "\n";
		SendRunServerInstancePacket(gameInstance->GetGameID(), i, midwareID, serverBorderStr);
	}
	PHYSICS_SERVER_ID_BUFFER += maxServer;
}

bool DistributedManager::SystemManager::CheckIsGameStartable(int gameInstanceID) {
	auto& serverList = GetPhysicsServerDataList(gameInstanceID);
	for (const auto& server : serverList) {
		if (!server->GetIsServerStarted() || !server->GetIsAllClientsConnectedToServer())
			return false;
	}

	return true;
}

std::vector<DistributedPhysicsServerData*>& DistributedManager::SystemManager::GetPhysicsServerDataList(
	int gameInstanceID) const {

	std::vector< DistributedPhysicsServerData*> dataList;

	for (const auto& serverData : mDistributedPhysicsServers) {
		if (serverData->GetGameInstanceID() == gameInstanceID) {
			dataList.push_back(serverData);
		}
	}

	return dataList;
}

int DistributedManager::SystemManager::GetAvailablePhysicsMidware() {
	const auto& firstMidware = mPhysicsServerMiddlwareRunningInstanceMap.begin();
	int minInstanceCount = firstMidware->second;
	int midwareID = firstMidware->first;
	for (const auto& midware : mPhysicsServerMiddlwareRunningInstanceMap) {
		if (midware.second < minInstanceCount) {
			minInstanceCount = midware.second;
			midwareID = midware.first;
		}
	}
	mPhysicsServerMiddlwareRunningInstanceMap[midwareID] = ++minInstanceCount;
	return midwareID;
}

 
NCL::GameInstance* DistributedManager::SystemManager::CreateNewGameInstance(int maxServer, int clientCount) {
	GameInstance* newGame = new GameInstance(++GAME_INSTANCE_ID_BUFFER, maxServer, PHYSICS_SERVER_ID_BUFFER, clientCount);
	mDistributedPhysicsManagerServer->AddGameInstance(newGame);

	return newGame;
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
