#include "DistributedGameServerManager.h"

#include <iostream>
#include <mutex>

#include "DistributedPacketSenderServer.h"
#include "GameWorld.h"
#include "NetworkObject.h"
#include "PhysicsObject.h"
#include "ServerWorldManager.h"
#include "TestObject.h"

namespace {
	constexpr int TEST_MAX_CLIENT = 1;
	constexpr int TEST_MAX_GAME_SERVER = 2;
}

DistributedGameServer::DistributedGameServerManager::DistributedGameServerManager(int serverId, const std::string& serverBordersStr) {
	mDistributedPacketSenderServer = nullptr;
	mThisDistributedPhysicsServer = nullptr;

	mGameServerId = serverId;

	NetworkBase::Initialise();
	PhyscisServerBorderData* serverBorderData = CreatePhysicsServerBorders(serverBordersStr);
	mServerWorldManager = new ServerWorldManager(mGameServerId, *serverBorderData, mPhysicsServerBorderMap);
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

		std::string serverName = "Server " + mGameServerId;

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

		std::thread senderThread(&DistributedGameServerManager::SendPacketsThread, this);
		senderThread.detach();

		std::cout << "Packet sender server started!\n";
		SendPacketSenderServerStartedPacket(mPacketSenderServerPort);
	}

	return mDistributedPacketSenderServer;
}

bool DistributedGameServer::DistributedGameServerManager::GetGameStarted() const {
	return mIsGameStarted;
}

void DistributedGameServer::DistributedGameServerManager::UpdateGameServerManager(float dt) {
	if (mThisDistributedPhysicsServer) {
		mThisDistributedPhysicsServer->UpdatePhysicsServer();
	}

	if (mDistributedPacketSenderServer) {
		mDistributedPacketSenderServer->UpdateServer();
	}

	for (auto* gameServerConnection : mDistributedPhysicsClients) {
		gameServerConnection->UpdateClient();
	}

	if (mIsGameStarted) {
		HandleObjectTransitions();

		mTimeToNextPacket -= dt;
		//std::cout << "Time to next packet: " << mTimeToNextPacket << "\n";
		//std::cout << "Packets To Snapshot: " << mPacketsToSnapshot << "\n";
		if (mTimeToNextPacket < 0) {
			mPacketsToSnapshot--;
			if (mPacketsToSnapshot < 0) {
				BroadcastSnapshot(false);
				//std::cout << "Full packet sent. Last sent full ID: " << mServerSideLastFullID << "\n";
				mPacketsToSnapshot = 5;
			}
			else {
				BroadcastSnapshot(true);
				//std::cout << "Delta packet. Last sent full ID: " << mServerSideLastFullID << "\n";
			}
			mTimeToNextPacket += 1.0f / 60.f; //20hz server/client update
		}


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
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::DistributedClientSync, this);

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
		HandleClientPlayerInputPacket(packet, source + 1);
		break;
	}
	case BasicNetworkMessages::StartDistributedPhysicsServer: {
		StartDistributedGameServerPacket* packet = static_cast<StartDistributedGameServerPacket*>(payload);
		HandleStartGameServerPacketReceived(packet);
		break;
	}
	case BasicNetworkMessages::GiveOwnershipOfObject: {
		auto* giveOwnershipPacket = static_cast<GiveOwnershipOfObjectPacket*>(payload);
		if (giveOwnershipPacket->newOwnerServerID == mGameServerId) {
			std::cout << "Transition Start Packet Received! \n";
			mServerWorldManager->HandleIncomingObjectCreation(giveOwnershipPacket->objectID);
		}

		break;
	}
	case BasicNetworkMessages::StartSimulatingObjectInServer: {
		if (auto* packet = static_cast<StartSimulatingObjectPacket*>(payload)) {
			std::cout << "Transition Finish Packet Received! \n";
			std::cout << "Received Full Packet ID: " << packet->lastFullState.stateID << "\n";
			if (packet->newOwnerServerID == mGameServerId) {

				mServerWorldManager->StartHandlingObject(packet);
			}
		}
		break;
	}
	case BasicNetworkMessages::DistributedClientSync: {
		if (auto* packet = static_cast<DistributedClientPacket*>(payload)) {
			for (const auto& testObj : mServerWorldManager->GetTestObjects()) {
				if (testObj->GetPlayerID() == packet->playerID) {
					testObj->ReceiveClientInputs(packet);
				}
			}
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
		if (o->WritePacket(&newPacket, deltaFrame, mServerSideLastFullID, mGameServerId)) {
			if (newPacket != nullptr) {
				//TODO(erendgrmnc): create a thread safe queue for servers to send state packets.
				std::lock_guard<std::mutex> lock(mPacketToSendQueueMutex);
				mPacketToSendQueue.push(newPacket);
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
	DistributedPhysicsServerAllClientsAreConnectedPacket packet(mGameServerId, true);
	mThisDistributedPhysicsServer->SendPacket(packet);
}

void DistributedGameServer::DistributedGameServerManager::SendPacketSenderServerStartedPacket(int port) const {
	DistributedPhysicsClientConnectedToManagerPacket packet(port, mGameServerId);
	std::cout << "Sending packet distributer started packet...\n";
	mThisDistributedPhysicsServer->SendPacket(packet);
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
	std::cout << "Paket Geldi\n";
	if (packet == nullptr) {
		std::cout << "Null geldi" << "\n";
		return;
	}

	if (mPhysicsServerBorderMap.size() != packet->totalServerCount) {
		for (int i = 0; i < packet->totalServerCount; i++) {
			if (!mPhysicsServerBorderMap.contains(i)) {
				PhyscisServerBorderData* serverBorderData = CreatePhysicsServerBorders(packet->borders[i]);
				std::pair<int, PhyscisServerBorderData*> pair = std::make_pair(packet->serverIDs[i], serverBorderData);
				mPhysicsServerBorderMap.insert(pair);
			}
		}
	}

	for (int i = 0; i < packet->currentServerCount; i++) {
		if (i == mGameServerId) {
			continue;
		}

		std::cout << "Received IP Address of Server( " << i << "): " << packet->createdServerIPs[i] << "\n";
		std::vector<char> ipOctets = IpToCharArray(packet->createdServerIPs[i]);
		//TODO(erendgrmnc): Add check if client already added.

		if (bool isConnectedToServer = ConnectServerToAnotherGameServer(127, 0, 0, 1, packet->serverPorts[i], i)) {
			std::cout << "Successfully connected to server " << i << "! \n";
		}
		else {
			std::cout << "Failed to connected to server " << i << "! \n";
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::HandleObjectTransitions() const {
	for (auto& networkObj : *mNetworkObjects) {
		if (networkObj->GetIsPredictedPosOutOfServer()) {
			std::cout << "Sending Start Transition Packet for object ID: " << networkObj->GetnetworkID() << "\n";
			SendStartTransitionPacket(*networkObj);
			networkObj->HandleAfterTransitionStarted();
		}
		else if (networkObj->GetIsActualPosOutOfServer()) {
			std::cout << "Sending Finish Transition Packet to server: " << networkObj->GetNewServerID() << "\n";
			SendFinishTransactionPacket(*networkObj);
			networkObj->HandleTransitionComplete();
			mServerWorldManager->HandleOutgoingObject(networkObj->GetnetworkID());
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::SendStartTransitionPacket(NetworkObject& obj) const {
	GiveOwnershipOfObjectPacket packet(obj.GetNewServerID(), obj.GetnetworkID());
	mDistributedPacketSenderServer->SendGlobalPacket(packet);
}

void DistributedGameServer::DistributedGameServerManager::SendFinishTransactionPacket(NetworkObject& obj) const {
	auto& gameObjectComp = obj.GetGameObject();

	NetworkState lastFullState = gameObjectComp.GetNetworkObject()->GetLatestNetworkState();
	lastFullState.position = gameObjectComp.GetTransform().GetPosition();
	lastFullState.orientation = gameObjectComp.GetTransform().GetOrientation();
	auto* testComp = dynamic_cast<TestObject*>(&gameObjectComp);

	StartSimulatingObjectPacket packet(obj.GetnetworkID(), obj.GetNewServerID(), lastFullState, *gameObjectComp.GetPhysicsObject());
	mDistributedPacketSenderServer->SendGlobalPacket(packet);
}

bool DistributedGameServer::DistributedGameServerManager::ConnectServerToAnotherGameServer(char a, char b, char c,
	char d, int port, int gameServerID) {

	std::cout << "Trying to connect Server on IP: " << a << "," << b << "," << c << "," << d << "/ port: " << port << "\n";
	auto* client = new NCL::CSC8503::GameClient();
	std::string name = "Server " + mGameServerId;

	const bool isConnected = client->Connect(a, b, c, d, port, name);

	if (isConnected) {
		client->RegisterPacketHandler(BasicNetworkMessages::StartSimulatingObjectInServer, this);
		client->RegisterPacketHandler(BasicNetworkMessages::GiveOwnershipOfObject, this);
	}

	mDistributedPhysicsClients.push_back(client);

	return isConnected;
}

NCL::Networking::DistributedPhysicsServerClient* DistributedGameServer::DistributedGameServerManager::GetDistributedPhysicsServer() const {
	return mThisDistributedPhysicsServer;
}

NCL::DistributedGameServer::ServerWorldManager* DistributedGameServer::DistributedGameServerManager::
GetServerWorldManager() const {
	return mServerWorldManager;
}
