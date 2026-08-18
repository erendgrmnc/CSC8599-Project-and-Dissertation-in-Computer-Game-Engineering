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

using namespace NCL;

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
	PhysicsServerBorderData* serverBorderData = CreatePhysicsServerBorders(serverBordersStr);
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

	// Published every update so the I4 accounting invariant can be checked from the
	// @@STAT stream without any extra instrumentation.
	Profiler::SetCommandsApplied(mCommandsApplied);
	Profiler::SetCommandsRelayed(mCommandsRelayed);
	Profiler::SetCommandsDuplicate(mCommandsDuplicate);
	Profiler::SetCommandsRejected(mCommandsRejected);
	Profiler::SetCommandsFannedOut(mCommandsFannedOut);
	Profiler::SetObjectsSpawned(mObjectsSpawned);
	Profiler::SetObjectsDestroyed(mObjectsDestroyed);
	Profiler::SetManifestEntriesSent(mManifestEntriesSent);

	RetryPendingPeers(dt);

	for (auto& gameServerConnection : mDistributedPhysicsClients) {
		gameServerConnection->client->UpdateClient();
	}

	if (mIsGameStarted) {
		FlushDelayedHandoffs();
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
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::DistributedClientSnapshotAck, this);
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::DistributedClientCommand, this);
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::DistributedServerCommandRelay, this);
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::DistributedObjectSpawned, this);
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::DistributedObjectDespawned, this);

	// Server-to-server traffic runs in BOTH directions over the peer mesh, and which
	// one a message type uses is decided purely by where its handler is registered:
	//
	//   sender's client link  ->  peer's PacketSenderServer   (relays; handlers here)
	//   sender's PacketSenderServer  ->  peer's client link   (handlers at the
	//                                    ConnectServerToAnotherGameServer site)
	//
	// Handoff was registered only on the client link, so it had to be broadcast from
	// the sender's own server. Making it directed means sending it down the relay
	// direction instead, which needs a handler on THIS side too. Registering it here
	// is what makes SendPacketToServer work for handoffs; without it the packet is
	// delivered by ENet and then silently dropped for want of a handler, which shows
	// up as hoSent > 0 with hoRecv == 0 and objects vanishing.
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::StartSimulatingObjectInServer, this);

	// One registration for the process. Adding a new interaction never touches
	// ReceivePacket - that is the point of the registry.
	NCL::Interaction::CommandRegistry::RegisterDefaults();

	// A late joiner gets one spawned-packet per object this server owns, sent to it
	// alone. Broadcasting the manifest instead would make every already-connected
	// client re-receive the entire world each time anyone joins.
	mDistributedPacketSenderServer->RegisterOnPeerJoinedEvent(
		[this](int peerNumber) { SendManifestToPeer(peerNumber); });

	std::function<void()> onAllClientsConnectedCallback = std::bind(&DistributedGameServerManager::SendAllClientsAreConnectedToPacketSenderServerPacket, this);
	mDistributedPacketSenderServer->RegisterOnAllClientsAreConnectedEvent(onAllClientsConnectedCallback);
}

void DistributedGameServer::DistributedGameServerManager::UpdateMinimumState() {
	//Periodically remove old data from the server
	int minID = INT_MAX;
	int maxID = 0; //we could use this to see if a player is lagging behind?

	// With no acknowledgements yet there is no safe baseline: pruning to INT_MAX
	// would discard the whole history and pinning the baseline to a state some
	// client has not seen would make its deltas unusable.
	if (mStateIDs.empty()) {
		mServerSideLastFullID = 0;
		return;
	}

	for (auto i : mStateIDs) {
		maxID = std::max(maxID, i.second);
	}

	// A client that stops acknowledging - because it disconnected without a clean
	// teardown, or has stalled - must not pin the baseline forever. Ignoring it costs
	// that client nothing permanent: it rejects deltas only until the next full
	// snapshot re-syncs its baseline, which happens at 10Hz.
	constexpr int kMaxAckLag = 30;
	for (auto i : mStateIDs) {
		if (maxID - i.second > kMaxAckLag) {
			continue;
		}
		minID = std::min(minID, i.second);
	}
	if (minID == INT_MAX) {
		minID = maxID;
	}

	// Deltas are broadcast to every client, so they must be encoded against a full
	// state that ALL of them hold - hence the minimum, not the newest.
	mServerSideLastFullID = minID;
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
	// Snapshot acknowledgement no longer piggybacks here: it keyed every client to
	// mStateIDs[0], so one arbitrary client's progress stood in for all of them.
	// See HandleClientSnapshotAckPacket.

	for (const auto& testObj : mServerWorldManager->GetTestObjects()) {
		if (testObj->GetPlayerID() == packet->playerID) {
			testObj->ReceiveClientInputs(packet);
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::HandleClientSnapshotAckPacket(
	DistributedClientSnapshotAckPacket* packet, int source) {
	if (packet->gameServerID != mGameServerID) {
		// The client broadcasts on a per-server link, but guard anyway: crediting
		// another server's snapshot IDs here would corrupt this server's baseline.
		return;
	}

	// Acks can arrive out of order on an unreliable path; never move a client's
	// high-water mark backwards.
	auto existing = mStateIDs.find(source);
	if (existing != mStateIDs.end() && existing->second >= packet->lastFullStateID) {
		return;
	}
	mStateIDs[source] = packet->lastFullStateID;

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
	case BasicNetworkMessages::DistributedClientCommand: {
		HandleClientCommandPacket(static_cast<DistributedClientCommandPacket*>(payload));
		break;
	}
	case BasicNetworkMessages::DistributedServerCommandRelay: {
		HandleServerCommandRelayPacket(static_cast<DistributedServerCommandRelayPacket*>(payload));
		break;
	}
	case BasicNetworkMessages::DistributedObjectSpawned: {
		HandleObjectSpawnedPacket(static_cast<DistributedObjectSpawnedPacket*>(payload));
		break;
	}
	case BasicNetworkMessages::DistributedObjectDespawned: {
		HandleObjectDespawnedPacket(static_cast<DistributedObjectDespawnedPacket*>(payload));
		break;
	}
	case BasicNetworkMessages::ClientPlayerInputState: {
		ClientPlayerInputPacket* packet = (ClientPlayerInputPacket*)payload;
		HandleClientPlayerInputPacket(packet, packet->playerID);
		break;
	}
	case BasicNetworkMessages::DistributedClientSnapshotAck: {
		auto* packet = static_cast<DistributedClientSnapshotAckPacket*>(payload);
		HandleClientSnapshotAckPacket(packet, source);
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
		if (networkObj->GetNetworkID() == packet->objectID) {
			//mServerWorldManager->HandleOutgoingObject(networkObj->GetNetworkID());
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

DistributedGameServer::PhysicsServerBorderData* DistributedGameServer::DistributedGameServerManager::
CreatePhysicsServerBorders(const std::string& borderString) {
	// Create a new PhysicsServerBorderData object
	PhysicsServerBorderData* borderData = new PhysicsServerBorderData();

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

	// stof, not stoi: the manager emits fractional borders whenever the world extent
	// is not divisible by the grid dimensions, and truncating them here desynchronised
	// this server's idea of its region from the manager's.
	borderData->minXVal = std::stof(xPart.substr(0, xSeparatorPos));
	borderData->maxXVal = std::stof(xPart.substr(xSeparatorPos + 1));

	borderData->minZVal = std::stof(zPart.substr(0, zSeparatorPos));
	borderData->maxZVal = std::stof(zPart.substr(zSeparatorPos + 1));

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
				PhysicsServerBorderData* serverBorderData = CreatePhysicsServerBorders(borderStr);
				std::pair<int, PhysicsServerBorderData*> pair = std::make_pair(packet->serverIDs[i], serverBorderData);
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

		// The IP/port arrays are in registration order, so i is NOT the peer's server
		// id. Labelling links by index made every id-based lookup miss - relays and
		// transition handshakes alike went to a link that did not exist.
		const int peerServerID = packet->connectedServerIDs[i];
		if (peerServerID < 0) {
			std::cout << "ERROR: no server id for entry " << i
				<< " (" << packet->createdServerIPs[i] << "); skipping peer link.\n";
			continue;
		}

		std::cout << "Received IP Address of Server( " << peerServerID << "): " << packet->createdServerIPs[i] << "\n";
		std::vector<char> ipOctets = IpToCharArray(packet->createdServerIPs[i]);

		bool isServerAdded = false;
		for (auto* gameServerConnection : mDistributedPhysicsClients) {
			if (gameServerConnection->serverID == peerServerID) {
				isServerAdded = true;
			}
		}

		if (!isServerAdded) {
			if (auto* serverConnection = ConnectServerToAnotherGameServer(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], packet->serverPorts[i], peerServerID)) {
				std::cout << "Successfully connected to server " << peerServerID << "! \n";
				mDistributedPhysicsClients.push_back(serverConnection);
			}
			else {
				std::cout << "Failed to connected to server " << peerServerID << "! \n";
			}
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::HandleObjectTransitions() const {
	for (auto& networkObj : *mNetworkObjects) {
		if (networkObj->GetIsActualPosOutOfServer()) {
			std::cout << "Sending Finish Transition Packet to server: " << networkObj->GetNewServerID() << "\n";
			// Release the object ONLY once the packet is actually on a link to the new
			// owner. The transition flag is left set on failure, so the next tick
			// retries rather than the object being lost to a link that was not up yet.
			if (!SendFinishTransactionPacket(*networkObj)) {
				continue;
			}
			mServerWorldManager->RecordHandoffSent();
			networkObj->HandleTransitionComplete();
			mServerWorldManager->HandleOutgoingObject(networkObj->GetNetworkID());
		}
	}
}

// Peer links are keyed by SERVER ID, never by array index. StartDistributedGameServerPacket
// carries two differently-indexed array families and connectedServerIDs[] is what maps
// registration order back to real ids; using the index instead silently sends to a link
// that does not exist. Solved once, here.
bool DistributedGameServer::DistributedGameServerManager::SendPacketToServer(int targetServerID,
	GamePacket& packet) const {
	for (const auto* connection : mDistributedPhysicsClients) {
		if (connection->serverID == targetServerID && connection->client != nullptr) {
			connection->client->SendReliablePacket(packet);
			return true;
		}
	}
	return false;
}

bool DistributedGameServer::DistributedGameServerManager::SendFinishTransactionPacket(NetworkObject& obj) const {
	auto& gameObjectComp = obj.GetGameObject();

	NetworkState lastFullState = gameObjectComp.GetNetworkObject()->GetLatestNetworkState();
	lastFullState.position = gameObjectComp.GetTransform().GetPosition();
	lastFullState.orientation = gameObjectComp.GetTransform().GetOrientation();
	auto* testComp = dynamic_cast<TestObject*>(&gameObjectComp);

	StartSimulatingObjectPacket packet(obj.GetNetworkID(), obj.GetNewServerID(), mGameServerID, lastFullState, *gameObjectComp.GetPhysicsObject());

	// Carry the object's control state across the border. Continuous input is never
	// relayed or replayed, so without this a driven avatar would stall on every
	// crossing until the client's next axis update reached the new owner.
	packet.mControllerPlayerID = gameObjectComp.GetControllerPlayerID();
	packet.mMoveAxis = gameObjectComp.GetMoveAxis();
	// The tick this release happened on. The receiver schedules application relative
	// to this rather than to arrival, which is what makes the handoff land on the
	// same tick in every run.
	packet.mSenderTick = static_cast<long long>(mServerWorldManager->GetTickCounter());
	// What the object is. Unused while every server holds a deactivated twin of
	// everything, but required the moment a receiver may not already have it.
	packet.mArchetypeID = mServerWorldManager->GetObjectArchetype(obj.GetNetworkID());

	const int delay = mServerWorldManager->GetHandoffDelayTicks();
	if (delay > 0) {
		// Held back deliberately. The object is still released locally on this tick,
		// so for the next `delay` ticks it exists on neither server - which is what
		// lets a destroy reach the new owner before the object does.
		mDelayedHandoffs.push_back(DelayedHandoff{ packet, delay });
		return true;
	}

	// Directed, not broadcast. This was SendGlobalReliablePacket, which was harmless
	// only because every server held a deactivated twin of every object and merely
	// ignored a handoff addressed elsewhere. Once a receiver BUILDS an object it does
	// not have, a broadcast would make every server construct its own copy - breaking
	// single ownership (I1) and putting the whole world back on every server (I6).
	if (!SendPacketToServer(packet.newOwnerServerID, packet)) {
		// Loud, and the caller keeps the object. There is no broadcast to fall back
		// on now, so releasing it here would destroy it outright.
		std::cout << "ERROR: no peer link to server " << packet.newOwnerServerID
			<< " for handoff of object " << packet.objectID
			<< " - object retained, will retry next tick.\n";
		return false;
	}
	return true;
}

void DistributedGameServer::DistributedGameServerManager::HandleClientCommandPacket(
	DistributedClientCommandPacket* packet) {
	if (packet == nullptr) {
		return;
	}

	const auto type = static_cast<NCL::Interaction::CommandType>(packet->commandType);
	const NCL::Interaction::IInteractionCommand* command =
		NCL::Interaction::CommandRegistry::Instance().Find(type);
	if (command == nullptr) {
		++mCommandsRejected;
		SendCommandAck(packet->sequence, packet->args.playerID, packet->args.targetObjectID,
			NCL::Interaction::CommandResult::Rejected, -1);
		return;
	}

	// Continuous input is idempotent state, so it is deliberately NOT sequenced -
	// dropping a duplicate axis update would be indistinguishable from dropping a
	// real one and would stall movement.
	const NCL::Interaction::CommandScope scope = command->GetScope(packet->args);
	if (!scope.isContinuous) {
		NCL::SequenceWindow& window = mClientCommandWindows[packet->args.playerID];
		if (!window.Accept(packet->sequence)) {
			++mCommandsDuplicate;
			SendCommandAck(packet->sequence, packet->args.playerID, packet->args.targetObjectID,
				NCL::Interaction::CommandResult::Duplicate, -1);
			return;
		}
	}

	DispatchCommand(type, packet->args, packet->args.playerID, packet->sequence);
}

void DistributedGameServer::DistributedGameServerManager::HandleServerCommandRelayPacket(
	DistributedServerCommandRelayPacket* packet) {
	if (packet == nullptr) {
		return;
	}

	// A relay is never re-relayed. Anything with hops already on it is a routing
	// loop, and dropping it loudly beats letting it circulate.
	if (packet->hopCount > 0) {
		++mCommandsRejected;
		std::cout << "ERROR: dropping relay with hopCount " << packet->hopCount
			<< " from server " << packet->originServerID << " - routing loop.\n";
		return;
	}

	NCL::SequenceWindow& window = mRelayWindows[packet->originServerID];
	if (!window.Accept(packet->originSequence)) {
		++mCommandsDuplicate;
		return;
	}

	// Anything this dispatch relays onward is a second hop and must be stamped as
	// such, or the guard above can never fire.
	mCurrentRelayHop = packet->hopCount + 1;
	DispatchCommand(static_cast<NCL::Interaction::CommandType>(packet->commandType),
		packet->args, packet->playerID, packet->clientSequence);
	mCurrentRelayHop = 0;
}

void DistributedGameServer::DistributedGameServerManager::DispatchCommand(
	NCL::Interaction::CommandType type, const NCL::Interaction::CommandArgs& args,
	int playerID, int clientSequence) {
	NCL::Interaction::IInteractionCommand* command =
		NCL::Interaction::CommandRegistry::Instance().Find(type);
	if (command == nullptr) {
		++mCommandsRejected;
		return;
	}

	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr) {
		++mCommandsRejected;
		return;
	}

	const NCL::Interaction::CommandResult result = command->Apply(*worldManager, args);

	switch (result) {
	case NCL::Interaction::CommandResult::Applied:  ++mCommandsApplied;  break;
	case NCL::Interaction::CommandResult::Relayed:  ++mCommandsRelayed;  break;
	default:                                        ++mCommandsRejected; break;
	}

	// Queued during Apply, sent now: a command must never re-enter the network layer
	// from inside a packet handler.
	DrainPendingRelays(playerID, clientSequence);
	DrainPendingSpawns();
	DrainPendingDespawns();

	// Only the server that APPLIED the command acks the client. A relaying server
	// stays silent so the client gets exactly one ack per command.
	if (result != NCL::Interaction::CommandResult::Relayed) {
		SendCommandAck(clientSequence, playerID, args.targetObjectID, result, -1);
	}
}

void DistributedGameServer::DistributedGameServerManager::DrainPendingRelays(int playerID,
	int clientSequence) {
	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr) {
		return;
	}

	ServerWorldManager::PendingRelay relay;
	while (worldManager->PopPendingRelay(relay)) {
		// A fan-out hop is not a misroute correction: it is an extra, intended
		// application of one command in another region. Counted apart so the I4
		// identity stays checkable.
		if ((relay.args.flags & static_cast<int>(NCL::Interaction::CommandFlags::AlreadyFannedOut)) != 0) {
			++mCommandsFannedOut;
		}

		DistributedServerCommandRelayPacket packet(
			static_cast<int>(relay.type),
			mGameServerID,
			++mRelaySequenceCounter,
			playerID,
			clientSequence,
			relay.args);
		packet.hopCount = mCurrentRelayHop;

		// Directed send over the existing peer mesh - the same lookup
		// SendTransactionHandshakePacket already does, so no new plumbing.
		bool sent = false;
		for (const auto* connection : mDistributedPhysicsClients) {
			if (connection->serverID == relay.targetServerID && connection->client != nullptr) {
				connection->client->SendReliablePacket(packet);
				sent = true;
				break;
			}
		}
		if (!sent) {
			// Silently dropping here would look identical to the command being
			// applied, which is exactly the hole invariant I4 exists to expose.
			++mCommandsRejected;
			std::cout << "ERROR: no peer link to server " << relay.targetServerID
				<< " for relay; have " << mDistributedPhysicsClients.size() << " link(s):";
			for (const auto* connection : mDistributedPhysicsClients) {
				std::cout << " " << connection->serverID;
			}
			std::cout << "\n";
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::DrainPendingSpawns() {
	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr || mDistributedPacketSenderServer == nullptr) {
		return;
	}

	ServerWorldManager::PendingSpawn spawn;
	while (worldManager->PopPendingSpawn(spawn)) {
		++mObjectsSpawned;

		// Broadcast reaches peers AND clients in one call, exactly as the handoff
		// packet does. Peers need it because StartHandlingObject requires a pool
		// entry to already exist; clients need it to build a replica.
		DistributedObjectSpawnedPacket packet(spawn.objectID, spawn.archetypeID,
			spawn.ownerServerID, spawn.spawnerPlayerID, spawn.position);
		mDistributedPacketSenderServer->SendGlobalReliablePacket(packet);
	}
}

void DistributedGameServer::DistributedGameServerManager::HandleObjectSpawnedPacket(
	DistributedObjectSpawnedPacket* packet) {
	if (packet == nullptr) {
		return;
	}
	// Our own broadcast coming back to us; the object already exists here.
	if (packet->ownerServerID == mGameServerID) {
		return;
	}

	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr) {
		return;
	}

	worldManager->CreateReplicatedSpawn(packet->objectID, packet->archetypeID,
		packet->ownerServerID, packet->spawnerPlayerID, packet->position);
}

void DistributedGameServer::DistributedGameServerManager::DrainPendingDespawns() {
	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr || mDistributedPacketSenderServer == nullptr) {
		return;
	}

	ServerWorldManager::PendingDespawn despawn;
	while (worldManager->PopPendingDespawn(despawn)) {
		++mObjectsDestroyed;

		// Explicit despawn rather than letting the object simply stop appearing in
		// snapshots: absence already means "not mine", so it cannot also mean
		// "destroyed" without making the two indistinguishable.
		DistributedObjectDespawnedPacket packet(despawn.objectID, despawn.reason,
			despawn.destroyerPlayerID);
		mDistributedPacketSenderServer->SendGlobalReliablePacket(packet);
	}
}

void DistributedGameServer::DistributedGameServerManager::HandleObjectDespawnedPacket(
	DistributedObjectDespawnedPacket* packet) {
	if (packet == nullptr) {
		return;
	}

	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr) {
		return;
	}

	worldManager->ApplyRemoteDespawn(packet->objectID, packet->reason, packet->destroyerPlayerID);
}

void DistributedGameServer::DistributedGameServerManager::FlushDelayedHandoffs() {
	if (mDelayedHandoffs.empty() || mDistributedPacketSenderServer == nullptr) {
		return;
	}

	for (auto entry = mDelayedHandoffs.begin(); entry != mDelayedHandoffs.end(); ) {
		if (--entry->ticksRemaining > 0) {
			++entry;
			continue;
		}
		// Directed, matching SendFinishTransactionPacket. The object was already
		// released when this was queued, so a missing link here DOES lose it - but
		// this path is fault injection (--handoff-delay-ticks), which must be 0 for
		// any measurement run, and losing the object is the effect being injected.
		if (!SendPacketToServer(entry->packet.newOwnerServerID, entry->packet)) {
			std::cout << "ERROR: no peer link to server " << entry->packet.newOwnerServerID
				<< " for delayed handoff of object " << entry->packet.objectID
				<< " - object LOST (it was released when the delay was queued).\n";
		}
		entry = mDelayedHandoffs.erase(entry);
	}
}

void DistributedGameServer::DistributedGameServerManager::RetryPendingPeers(float dt) {
	if (mPendingPeers.empty()) {
		return;
	}

	mPeerRetryTimer -= dt;
	if (mPeerRetryTimer > 0.0f) {
		return;
	}
	mPeerRetryTimer = 0.5f;

	for (auto entry = mPendingPeers.begin(); entry != mPendingPeers.end(); ) {
		auto* connection = ConnectServerToAnotherGameServer(
			entry->ip[0], entry->ip[1], entry->ip[2], entry->ip[3], entry->port, entry->serverID);
		if (connection == nullptr) {
			++entry;
			continue;
		}
		std::cout << "Connected to server " << entry->serverID << " on retry.\n";
		mDistributedPhysicsClients.push_back(connection);
		entry = mPendingPeers.erase(entry);
	}
}

void DistributedGameServer::DistributedGameServerManager::SendManifestToPeer(int peerNumber) {
	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr || mDistributedPacketSenderServer == nullptr) {
		return;
	}

	const auto manifest = worldManager->BuildOwnedObjectManifest();
	for (const auto& entry : manifest) {
		DistributedObjectSpawnedPacket packet(entry.objectID, entry.archetypeID,
			mGameServerID, -1, entry.position);
		if (mDistributedPacketSenderServer->SendPacketToPeer(peerNumber, packet)) {
			++mManifestEntriesSent;
		}
	}

	if (!manifest.empty()) {
		std::cout << "Sent late-join manifest of " << manifest.size()
			<< " owned objects to peer " << peerNumber << "\n";
	}
}

void DistributedGameServer::DistributedGameServerManager::SendCommandAck(int sequence, int playerID,
	int targetObjectID, NCL::Interaction::CommandResult result, int correctedServerID) {
	DistributedCommandAckPacket packet(sequence, playerID, targetObjectID,
		static_cast<int>(result), correctedServerID);

	// Broadcast to this server's connected clients, matching how snapshots are sent.
	// The client filters on playerID; a directed per-peer send needs retained
	// ENetPeer* work that belongs with the late-join manifest.
	if (mDistributedPacketSenderServer != nullptr) {
		mDistributedPacketSenderServer->SendGlobalReliablePacket(packet);
	}
}

void DistributedGameServer::DistributedGameServerManager::SendTransactionHandshakePacket(int senderServerID, int networkID) const {
	StartSimulatingObjectReceivedPacket packet(networkID, mGameServerID);

	std::cout << "Sending transition handshake packet to server with ID: " << senderServerID << "\n";

	if (!SendPacketToServer(senderServerID, packet)) {
		std::cout << "ERROR: no peer link to server " << senderServerID
			<< " for transition handshake of object " << networkID << "\n";
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
		// Without this, relayed commands arrive on the peer link and are silently
		// dropped - the misroute path would look like it simply lost the command.
		client->RegisterPacketHandler(BasicNetworkMessages::DistributedServerCommandRelay, this);
		// Spawns are broadcast on the owner's sender server, so a peer receives them
		// through its outbound link exactly as it receives a handoff.
		client->RegisterPacketHandler(BasicNetworkMessages::DistributedObjectSpawned, this);
		client->RegisterPacketHandler(BasicNetworkMessages::DistributedObjectDespawned, this);
	}

	if (!isConnected) {
		// Reported as failure rather than returned anyway. Previously a failed
		// connect still produced a GameServerConnection and the caller logged
		// "Successfully connected", so the peer silently never existed - and the
		// server it should have connected to waited forever for its peer count.
		std::cout << "Failed to connect to server " << gameServerID << " on port " << port
			<< " - will retry.\n";
		delete client;
		return nullptr;
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
