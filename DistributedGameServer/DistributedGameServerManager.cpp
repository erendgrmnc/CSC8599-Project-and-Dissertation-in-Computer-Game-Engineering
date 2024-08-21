#include "DistributedGameServerManager.h"

#include <iostream>
#include <mutex>

#include "DistributedPacketSenderServer.h"
#include "GameWorld.h"
#include "NetworkObject.h"
#include "PhysicsObject.h"
#include "Profiler.h"
#include "ServerWorldManager.h"
#include "TestObject.h"
#include <DistributedSystemCommonFiles/DistributedUtils.h>

namespace {
	constexpr int TEST_MAX_CLIENT = 10;
	constexpr int TEST_MAX_GAME_SERVER = 10;
}

DistributedGameServer::GameServerConnection::GameServerConnection(int serverID, GameClient* client) {
	this->serverID = serverID;
	this->client = client;
}

DistributedGameServer::DistributedGameServerManager::DistributedGameServerManager(int serverID, int gameInstanceID, const std::string& serverBordersStr) {
	mDistributedPacketSenderServer = nullptr;
	mThisDistributedPhysicsServer = nullptr;

	mGameServerID = serverID;
	mGameInstanceID = gameInstanceID;

	NetworkBase::Initialise();
	PhyscisServerBorderData* serverBorderData = CreatePhysicsServerBorders(serverBordersStr);
	mServerWorldManager = new ServerWorldManager(mGameServerID, *serverBorderData, mPhysicsServerBorderMap);
	mNetworkObjects = mServerWorldManager->GetNetworkObjects();

	bool isEmpty = mPacketToSendQueue.empty();
	mTimeToNextPacket = 0.0f;
	mPacketsToSnapshot = -1;
	mServerSideLastFullID = 0;
}

DistributedGameServer::DistributedGameServerManager::~DistributedGameServerManager() {
	delete mServerWorldManager;
	delete mThisDistributedPhysicsServer;
}

bool DistributedGameServer::DistributedGameServerManager::StartDistributedGameServer(char a, char b, char c, char d, int port) {
	mThisDistributedPhysicsServer = new NCL::Networking::DistributedPhysicsServerClient();
	if (mThisDistributedPhysicsServer) {
		std::function<void()> onConnectedToDistributedManager = [this] { StartDistributedPacketSenderServer(); };
		mThisDistributedPhysicsServer->RegisterOnConnectedToDistributedManagerEvent(onConnectedToDistributedManager);

		std::string serverName = "Server " + mGameServerID;

		mThisDistributedPhysicsServer->Connect(a, b, c, d, port, serverName);
		RegisterGameServerPackets();
	}
	return mThisDistributedPhysicsServer;
}

bool DistributedGameServer::DistributedGameServerManager::StartDistributedPacketSenderServer() {
	mMaxGameClientsToConnectPacketSender = TEST_MAX_CLIENT + (TEST_MAX_GAME_SERVER - 1);
	mPacketSenderServerPort = (mThisDistributedPhysicsServer->GetPeerID() * 10) + 1000;
	mDistributedPacketSenderServer = new NCL::Networking::DistributedPacketSenderServer(mPacketSenderServerPort, mMaxGameClientsToConnectPacketSender);
	if (mDistributedPacketSenderServer) {
		RegisterPacketSenderServerPackets();

		//std::thread senderThread(&DistributedGameServerManager::SendPacketsThread, this);
		//senderThread.detach();

		std::cout << "Packet sender server started!\n";
		SendPacketSenderServerStartedPacket(mPacketSenderServerPort);
	}

	return mDistributedPacketSenderServer;
}

bool DistributedGameServer::DistributedGameServerManager::GetGameStarted() const {
	return mIsGameStarted;
}

void DistributedGameServer::DistributedGameServerManager::UpdateGameServerManager(float dt) {
	std::chrono::steady_clock::time_point start;
	std::chrono::steady_clock::time_point end;
	std::chrono::duration<double, std::milli> timeTaken;

	if (mThisDistributedPhysicsServer) {
		mThisDistributedPhysicsServer->UpdatePhysicsServer();
	}

	if (mDistributedPacketSenderServer) {
		mDistributedPacketSenderServer->UpdateServer();
	}

	for (auto& gameServerConnection : mDistributedPhysicsClients) {
		gameServerConnection->client->UpdateClient();
	}

	if (mIsGameStarted) {
		HandleObjectTransitions();

		mTimeToNextPacket -= dt;

		if (mTimeToNextPacket < 0) {
			mPacketsToSnapshot--;
			if (mPacketsToSnapshot < 0) {
				start = std::chrono::high_resolution_clock::now();
				BroadcastSnapshot(false);
				end = std::chrono::high_resolution_clock::now();
				timeTaken = end - start;
				Profiler::SetLastFullSnapshotTime(timeTaken.count());
				mPacketsToSnapshot = 5;
			}
			else {
				start = std::chrono::high_resolution_clock::now();
				BroadcastSnapshot(true);
				end = std::chrono::high_resolution_clock::now();
				timeTaken = end - start;
				Profiler::SetLastDeltaSnapshotTime(timeTaken.count());
			}
			mTimeToNextPacket += 1.0f / 60.f; //20hz server/client update
		}
	}

	mDebugTimer -= dt;

	if (mDebugTimer <= 0.f) {

		std::cout << "Last Received Client ID " << mStateIDs[0] << "\n";
		mDebugTimer = 5.f;
	}
}

void DistributedGameServer::DistributedGameServerManager::RegisterGameServerPackets() {
	mThisDistributedPhysicsServer->RegisterPacketHandler(String_Message, this);
	mThisDistributedPhysicsServer->RegisterPacketHandler(BasicNetworkMessages::GameStartState, this);
	mThisDistributedPhysicsServer->RegisterPacketHandler(BasicNetworkMessages::StartDistributedPhysicsServer, this);
}

void DistributedGameServer::DistributedGameServerManager::RegisterPacketSenderServerPackets() {
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::ClientPlayerInputState, this);
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::ClientInit, this);
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::DistributedClientConnectToPhysicsServer, this);
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::StartSimulatingObjectInServerReceived, this);

	std::function<void()> onAllClientsConnectedCallback = std::bind(&DistributedGameServerManager::SendAllClientsAreConnectedToPacketSenderServerPacket, this);
	mDistributedPacketSenderServer->RegisterOnAllClientsAreConnectedEvent(onAllClientsConnectedCallback);
}

void DistributedGameServer::DistributedGameServerManager::UpdateMinimumState() {
	//Periodically remove old data from the server
	int minID = INT_MAX;
	int maxID = 0; //we could use this to see if a player is lagging behind?

	for (auto i : mStateIDs) {
		minID = std::min(minID, i.second);
		maxID = std::max(maxID, i.second);
	}
	//every client has acknowledged reaching at least state minID
	//so we can get rid of any old states!
	std::vector<GameObject*>::const_iterator first;
	std::vector<GameObject*>::const_iterator last;
	mServerWorldManager->GetGameWorld()->GetObjectIterators(first, last);
	for (auto i = first; i != last; ++i) {
		NetworkObject* o = (*i)->GetNetworkObject();
		if (!o) {
			continue;
		}
		o->UpdateStateHistory(minID); //clear out old states so they arent taking up memory...
	}
}

void DistributedGameServer::DistributedGameServerManager::HandleClientPlayerInputPacket(ClientPlayerInputPacket* packet,
	int playerPeerID) {
	//int playerIndex = GetPlayerPeerID(playerPeerId);
	//auto* playerToHandle = mServerPlayers[playerIndex];

	//playerToHandle->SetPlayerInput(clientPlayerInputPacket->playerInputs);
	mServerSideLastFullID = packet->lastId;
	mStateIDs[0] = mServerSideLastFullID;
	UpdateMinimumState();

	for (const auto& testObj : mServerWorldManager->GetTestObjects()) {
		if (testObj->GetPlayerID() == packet->playerID) {
			testObj->ReceiveClientInputs(packet);
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::ReceivePacket(int type, GamePacket* payload, int source) {
	switch (type) {
	case BasicNetworkMessages::String_Message: {
		StringPacket* packet = static_cast<StringPacket*>(payload);
		std::cout << packet->stringData << "\n";
		break;
	}
	case BasicNetworkMessages::GameStartState: {
		GameStartStatePacket* packet = static_cast<GameStartStatePacket*>(payload);
		HandleGameStarted(packet);
		break;
	}
	case BasicNetworkMessages::ClientPlayerInputState: {
		ClientPlayerInputPacket* packet = (ClientPlayerInputPacket*)payload;
		HandleClientPlayerInputPacket(packet, packet->playerID);
		break;
	}
	case BasicNetworkMessages::StartDistributedPhysicsServer: {
		StartDistributedGameServerPacket* packet = static_cast<StartDistributedGameServerPacket*>(payload);
		HandleStartGameServerPacketReceived(packet);
		break;
	}
	case BasicNetworkMessages::StartSimulatingObjectInServer: {
		if (auto* packet = static_cast<StartSimulatingObjectPacket*>(payload)) {
			std::cout << "Transition Finish Packet Received! \n";
			std::cout << "Received Full Packet ID: " << packet->lastFullState.stateID << "\n";
			if (packet->newOwnerServerID == mGameServerID) {
				if (mServerWorldManager->StartHandlingObject(packet)) {
					//mStateIDs[0] = packet->lastFullState.stateID;
					SendTransactionHandshakePacket(packet->senderServerID, packet->objectID);
				}
				else {
					std::cout << "Failed to receive object " << packet->objectID << "from server " << packet->senderServerID << std::endl;
				}
			}
		}
		break;
	}
	case BasicNetworkMessages::StartSimulatingObjectInServerReceived: {
		if (auto* packet = static_cast<StartSimulatingObjectReceivedPacket*>(payload)) {
			std::cout << "Transition handshake received from " << packet->newOwnerServerID << " for network object ID " << packet->objectID;
			HandleTransitionHandshakePacketReceived(packet);
		}

		break;
	}
	default:
		std::cout << "Received unknown packet. Type: " << payload->type << std::endl;
		break;
	}
}

void DistributedGameServer::DistributedGameServerManager::BroadcastSnapshot(bool deltaFrame) {
	std::vector<GameObject*>::const_iterator first;
	std::vector<GameObject*>::const_iterator last;

	mServerWorldManager->GetGameWorld()->GetObjectIterators(first, last);

	for (auto i = first; i != last; ++i) {
		NetworkObject* o = (*i)->GetNetworkObject();
		if (!o || !(*i)->IsNetworkActive()) {
			continue;
		}
		//TODO - you'll need some way of determining
		//when a player has sent the server an acknowledgement
		//and store the lastID somewhere. A map between player
		//and an int could work, or it could be part of a 
		//NetworkPlayer struct. 
		GamePacket* newPacket = nullptr;
		if (o->WritePacket(&newPacket, deltaFrame, mServerSideLastFullID, mGameServerID)) {
			if (newPacket != nullptr) {
				//TODO(erendgrmnc): create a thread safe queue for servers to send state packets.
				std::lock_guard<std::mutex> lock(mPacketToSendQueueMutex);
				mDistributedPacketSenderServer->SendGlobalPacket(*newPacket);
			}
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::SendPacketsThread() {
	while (mDistributedPacketSenderServer) {
		std::lock_guard<std::mutex> lock(mPacketToSendQueueMutex);
		if (mPacketToSendQueue.size() > 1 && !mPacketToSendQueue.empty() && mIsGameStarted) {

			GamePacket* packet = mPacketToSendQueue.front();
			if (packet) {
				mDistributedPacketSenderServer->SendGlobalPacket(*packet);
				mPacketToSendQueue.pop();
			}
		}
	}

	std::cout << "Packet sender server is not available anymore" << std::endl;
}

void DistributedGameServer::DistributedGameServerManager::HandleGameStarted(
	CSC8503::GameStartStatePacket* gameStartPacket) {
	mIsGameStarted = gameStartPacket->isGameStarted;
	StartGame();
	std::cout << "Game started packet received, starting server." << "\n";
}

void DistributedGameServer::DistributedGameServerManager::StartGame() {

}

void DistributedGameServer::DistributedGameServerManager::SendAllClientsAreConnectedToPacketSenderServerPacket() const {
	DistributedPhysicsServerAllClientsAreConnectedPacket packet(mGameInstanceID, mGameServerID, true);
	mThisDistributedPhysicsServer->SendPacket(packet);
}

void DistributedGameServer::DistributedGameServerManager::SendPacketSenderServerStartedPacket(int port) const {
	std::string ipAddressOfMachine = DistributedUtils::GetMachineIPV4Address();
	std::cout << "IPV4 Address of the machine: " << ipAddressOfMachine << std::endl;
	DistributedPhysicsClientConnectedToManagerPacket packet(port, mGameServerID, mGameInstanceID, ipAddressOfMachine);
	std::cout << "Sending packet distributer started packet...\n";
	mThisDistributedPhysicsServer->SendPacket(packet);
}

void DistributedGameServer::DistributedGameServerManager::HandleTransitionHandshakePacketReceived(
	StartSimulatingObjectReceivedPacket* packet) {
	for (auto& networkObj : *mNetworkObjects) {
		if (networkObj->GetnetworkID() == packet->objectID) {
			//mServerWorldManager->HandleOutgoingObject(networkObj->GetnetworkID());
		}

	}

}

std::vector<char> DistributedGameServer::DistributedGameServerManager::IpToCharArray(const std::string& ipAddress) {
	std::vector<std::string> ip_bytes;
	std::stringstream ss(ipAddress);
	std::string segment;

	while (std::getline(ss, segment, '.')) {
		ip_bytes.push_back(segment);
	}

	if (ip_bytes.size() != 4) {
		throw std::invalid_argument("Invalid IPv4 address format");
	}

	std::vector<char> ip_packed;
	for (const std::string& byte_str : ip_bytes) {
		int byte_value = std::stoi(byte_str);
		if (byte_value < 0 || byte_value > 255) {
			throw std::invalid_argument("Invalid IPv4 address format");
		}
		ip_packed.push_back(static_cast<char>(byte_value));
	}

	return ip_packed;
}

DistributedGameServer::PhyscisServerBorderData* DistributedGameServer::DistributedGameServerManager::
CreatePhysicsServerBorders(const std::string& borderString) {
	// Create a new PhyscisServerBorderData object
	PhyscisServerBorderData* borderData = new PhyscisServerBorderData();

	// Find the position of the '|' separator
	size_t separatorPos = borderString.find('|');
	if (separatorPos == std::string::npos) {
		// Handle the error if the separator is not found
		return nullptr;
	}

	// Extract the minXVal/maxXVal part of the string
	std::string xPart = borderString.substr(0, separatorPos);
	// Extract the minZVal/maxZVal part of the string
	std::string zPart = borderString.substr(separatorPos + 1);

	// Find the position of the '/' separator in xPart
	size_t xSeparatorPos = xPart.find('/');
	if (xSeparatorPos == std::string::npos) {
		// Handle the error if the separator is not found
		return nullptr;
	}

	// Find the position of the '/' separator in zPart
	size_t zSeparatorPos = zPart.find('/');
	if (zSeparatorPos == std::string::npos) {
		// Handle the error if the separator is not found
		return nullptr;
	}

	// Parse the minXVal and maxXVal
	borderData->minXVal = std::stoi(xPart.substr(0, xSeparatorPos));
	borderData->maxXVal = std::stoi(xPart.substr(xSeparatorPos + 1));

	// Parse the minZVal and maxZVal
	borderData->minZVal = std::stoi(zPart.substr(0, zSeparatorPos));
	borderData->maxZVal = std::stoi(zPart.substr(zSeparatorPos + 1));

	return borderData;
}

void DistributedGameServer::DistributedGameServerManager::HandleStartGameServerPacketReceived(
	StartDistributedGameServerPacket* packet) {

	if (packet->gameInstanceID != mGameInstanceID) {
		return;
	}

	int maxClient = (packet->totalServerCount - 1) + packet->clientsToConnect;

	std::cout << "Max client for packet sender server: " << maxClient << "\n";
	mDistributedPacketSenderServer->SetMaxClients(maxClient);

	if (mPhysicsServerBorderMap.size() != packet->totalServerCount) {
		for (int i = 0; i < packet->totalServerCount; i++) {
			if (!mPhysicsServerBorderMap.contains(i)) {
				std::string borderStr(packet->borders[i]);
				std::cout << "Received Border for server " << i << " " << borderStr << "\n";
				PhyscisServerBorderData* serverBorderData = CreatePhysicsServerBorders(borderStr);
				std::pair<int, PhyscisServerBorderData*> pair = std::make_pair(packet->serverIDs[i], serverBorderData);
				mPhysicsServerBorderMap.insert(pair);
			}
		}
	}
	if (!mIsPlayerObjectsCreated) {
		mServerWorldManager->CreatePlayerObjects(packet->clientsToConnect, packet->objectsPerPlayer);
		mIsPlayerObjectsCreated = true;
	}

	for (int i = 0; i < packet->currentServerCount; i++) {
		if (packet->createdServerIPs[i] == DistributedUtils::GetMachineIPV4Address() && mPacketSenderServerPort == packet->serverPorts[i]) {
			continue;
		}

		std::cout << "Received IP Address of Server( " << i << "): " << packet->createdServerIPs[i] << "\n";
		std::vector<char> ipOctets = IpToCharArray(packet->createdServerIPs[i]);
		//TODO(erendgrmnc): Add check if client already added.

		bool isServerAdded = false;
		for (auto* gameServerConnection : mDistributedPhysicsClients) {
			if (gameServerConnection->serverID == i) {
				isServerAdded = true;
			}
		}

		if (!isServerAdded) {
			if (auto* serverConnection = ConnectServerToAnotherGameServer(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], packet->serverPorts[i], i)) {
				std::cout << "Successfully connected to server " << i << "! \n";
				mDistributedPhysicsClients.push_back(serverConnection);
			}
			else {
				std::cout << "Failed to connected to server " << i << "! \n";
			}
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::HandleObjectTransitions() const {
	for (auto& networkObj : *mNetworkObjects) {
		if (networkObj->GetIsActualPosOutOfServer()) {
			std::cout << "Sending Finish Transition Packet to server: " << networkObj->GetNewServerID() << "\n";
			SendFinishTransactionPacket(*networkObj);
			networkObj->HandleTransitionComplete();
			mServerWorldManager->HandleOutgoingObject(networkObj->GetnetworkID());
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::SendFinishTransactionPacket(NetworkObject& obj) const {
	auto& gameObjectComp = obj.GetGameObject();

	NetworkState lastFullState = gameObjectComp.GetNetworkObject()->GetLatestNetworkState();
	lastFullState.position = gameObjectComp.GetTransform().GetPosition();
	lastFullState.orientation = gameObjectComp.GetTransform().GetOrientation();
	auto* testComp = dynamic_cast<TestObject*>(&gameObjectComp);

	StartSimulatingObjectPacket packet(obj.GetnetworkID(), obj.GetNewServerID(), mGameServerID, lastFullState, *gameObjectComp.GetPhysicsObject());
	mDistributedPacketSenderServer->SendGlobalReliablePacket(packet);
}

void DistributedGameServer::DistributedGameServerManager::SendTransactionHandshakePacket(int senderServerID, int networkID) const {
	StartSimulatingObjectReceivedPacket packet(networkID, mGameServerID);

	std::cout << "Sending transition handshake packet to server with ID: " << senderServerID << "\n";

	for (const auto* connection : mDistributedPhysicsClients) {
		if (connection->serverID == senderServerID) {
			connection->client->SendReliablePacket(packet);
			return;
		}
	}
}

DistributedGameServer::GameServerConnection* DistributedGameServer::DistributedGameServerManager::ConnectServerToAnotherGameServer(char a, char b, char c,
	char d, int port, int gameServerID) {
	std::cout << "Trying to connect Server on IP: " << a << "," << b << "," << c << "," << d << "/ port: " << port << "\n";
	auto* client = new NCL::CSC8503::GameClient();
	std::string name = "Server " + mGameServerID;

	const bool isConnected = client->Connect(a, b, c, d, port, name);

	if (isConnected) {
		client->RegisterPacketHandler(BasicNetworkMessages::StartSimulatingObjectInServer, this);
	}

	GameServerConnection* connection = new GameServerConnection(gameServerID, client);
	return connection;
}

NCL::Networking::DistributedPhysicsServerClient* DistributedGameServer::DistributedGameServerManager::GetDistributedPhysicsServer() const {
	return mThisDistributedPhysicsServer;
}

NCL::DistributedGameServer::ServerWorldManager* DistributedGameServer::DistributedGameServerManager::
GetServerWorldManager() const {
	return mServerWorldManager;
}
