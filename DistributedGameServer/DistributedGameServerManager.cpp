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
	// Peer slots the packet-sender host is created with.
	//
	// This used to be TEST_MAX_CLIENT + (TEST_MAX_GAME_SERVER - 1) = 19, and it was a
	// hard cap on instance size that had nothing to do with the wire format: an ENet
	// host's peer capacity is fixed at enet_host_create, so on a 24-server instance
	// the 20th peer onwards was simply refused, every server's readiness test stayed
	// false, and the game never started - with no error anywhere.
	//
	// Sized generously instead. An ENetPeer is on the order of a kilobyte, so 256
	// slots costs a few hundred KB per server, against an instance size limit that
	// was previously invisible. The logical bound is still set from the real instance
	// size by SetMaxClients; this is only the ceiling that bound may reach.
	constexpr int PACKET_SENDER_PEER_SLOTS = 256;
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
	mMaxGameClientsToConnectPacketSender = PACKET_SENDER_PEER_SLOTS;
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
	// Drained per tick, not only from DispatchCommand. Spawns and destroys used to be
	// drained solely at the end of a client command, on the assumption that a command
	// is the only thing that creates an object. The injection workload breaks that:
	// it spawns from the world manager on a schedule, with no command anywhere, so on
	// a run with no client traffic mPendingSpawns grew for the whole run - every
	// injected object went uncounted (objSpawned stayed 0, which made the conservation
	// invariant read the entire population as unaccounted) and no spawn broadcast was
	// ever sent, so a connected client would never have learned the objects existed.
	DrainPendingSpawns();
	DrainPendingDespawns();

	Profiler::SetCommandsApplied(mCommandsApplied);
	Profiler::SetCommandsRelayed(mCommandsRelayed);
	Profiler::SetCommandsDuplicate(mCommandsDuplicate);
	Profiler::SetCommandsRejected(mCommandsRejected);
	Profiler::SetCommandsFannedOut(mCommandsFannedOut);
	Profiler::SetObjectsSpawned(mObjectsSpawned);
	Profiler::SetObjectsDestroyed(mObjectsDestroyed);
	Profiler::SetManifestEntriesSent(mManifestEntriesSent);
	Profiler::SetHaloUpdatesSent(mHaloUpdatesSent);
	Profiler::SetHaloObjectsSent(mHaloObjectsSent);
	Profiler::SetHaloUpdatesReceived(mHaloUpdatesReceived);
	Profiler::SetHaloObjectsReceived(mHaloObjectsReceived);
	Profiler::SetSnapshotsSent(mSnapshotsSent);
	Profiler::SetSnapshotsSuppressed(mSnapshotsSuppressed);

	// Tell every peer server not to send us snapshots. A server registers no handler
	// for Full_State or Delta_State - it learns about its neighbours' objects through
	// the halo band - so everything it was sent was decoded and discarded. With two
	// servers and one client that was two thirds of all snapshot traffic.
	//
	// On a timer rather than once at connect: ENet's handshake is not complete when
	// Connect returns, so a declaration sent there is queued against a peer that does
	// not exist yet and is silently dropped. Repeating it costs one packet per peer
	// per second and cannot be missed.
	mPeerInterestDeclareTimer -= dt;
	if (mPeerInterestDeclareTimer <= 0.0f) {
		mPeerInterestDeclareTimer = 1.0f;
		for (auto* connection : mDistributedPhysicsClients) {
			if (connection != nullptr && connection->client != nullptr) {
				DistributedClientInterestPacket noSnapshots(mGameServerID,
					Maths::Vector3(0, 0, 0), -1.0f);
				connection->client->SendReliablePacket(noSnapshots);
			}
		}
	}

	RetryPendingPeers(dt);

	for (auto& gameServerConnection : mDistributedPhysicsClients) {
		gameServerConnection->client->UpdateClient();
	}

	if (mIsGameStarted) {
		// Reported to the manager, which owns the partition and may move it. Emitted
		// on a tick schedule the servers agree on, so the manager can decide from
		// reports that all describe the same simulated moment.
		long long reportTick = 0;
		int reportOwned = 0;
		long long reportContacts = 0;
		float reportMinX = 0.0f;
		float reportMaxX = 0.0f;
		int reportBuckets[ServerWorldManager::LOAD_REPORT_BUCKETS] = {};
		if (mServerWorldManager->TakeLoadReport(reportTick, reportOwned, reportContacts,
			reportMinX, reportMaxX, reportBuckets) && mThisDistributedPhysicsServer != nullptr) {
			DistributedServerLoadReportPacket report(mGameServerID, mGameInstanceID,
				reportTick, reportOwned, reportContacts, reportMinX, reportMaxX, reportBuckets);
			// Reliable: a lost report stalls the whole round, because the manager waits
			// for one from every server before deciding.
			mThisDistributedPhysicsServer->SendReliablePacket(report);
		}

		FlushDelayedHandoffs();
		HandleObjectTransitions();
		PublishHaloBand();

		// Resends for transfers that were never acknowledged. Same reliable path as
		// the original send. A receiver that STILL HOLDS the object re-applies it
		// harmlessly and acks, which discharges custody - but one that has since handed
		// the object onward re-installs an object that now lives elsewhere, so resends
		// are bounded by --handoff-max-attempts rather than repeated indefinitely (see
		// NCL::Distributed::DecideCustody).
		//
		// The verdict is fed straight back into custody, and it is deliberately not
		// just "the send returned false": a refused send with a peer link still in
		// place is ENet's outgoing queue filling up under overload, which is exactly
		// the condition a timeout-based reclaim used to misread as death. Only a
		// MISSING peer link authorises a reclaim. ServerWorldManager cannot see either
		// fact itself - it never touches the network layer.
		StartSimulatingObjectPacket resend;
		while (mServerWorldManager->PopHandoffResend(resend)) {
			const bool delivered = SendPacketToServer(resend.newOwnerServerID, resend);
			const bool peerLinkGone = !delivered && !HasPeerLink(resend.newOwnerServerID);
			mServerWorldManager->RecordHandoffResendResult(resend.objectID, peerLinkGone);
		}

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
			mTimeToNextPacket += 1.0f / 60.f; // 60hz, one full snapshot in six
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
	// From the manager, which owns the partition.
	mThisDistributedPhysicsServer->RegisterPacketHandler(BasicNetworkMessages::DistributedRepartition, this);
	mThisDistributedPhysicsServer->RegisterPacketHandler(BasicNetworkMessages::DistributedServerRegistry, this);
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
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::DistributedHaloUpdate, this);
	mDistributedPacketSenderServer->RegisterPacketHandler(BasicNetworkMessages::DistributedClientInterest, this);

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
	case BasicNetworkMessages::DistributedHaloUpdate: {
		HandleHaloUpdatePacket(static_cast<HaloUpdatePacket*>(payload));
		break;
	}
	case BasicNetworkMessages::DistributedRepartition: {
		HandleRepartitionPacket(static_cast<DistributedRepartitionPacket*>(payload));
		break;
	}
	case BasicNetworkMessages::DistributedServerRegistry: {
		HandleServerRegistryPacket(static_cast<DistributedServerRegistryPacket*>(payload));
		break;
	}
	case BasicNetworkMessages::DistributedClientInterest: {
		HandleClientInterestPacket(static_cast<DistributedClientInterestPacket*>(payload), source);
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

// A client's declared area of interest. Replaces the previous arrangement, which was
// that every client was told about every object this server owns.
void DistributedGameServer::DistributedGameServerManager::HandleClientInterestPacket(
	DistributedClientInterestPacket* packet, int source) {
	if (packet == nullptr) {
		return;
	}
	// source + 1, matching the key SendPacketToPeer uses: the event loop stores peer
	// handles under incomingPeerID + 1 but hands ProcessPacket the raw id.
	PeerInterest& interest = mPeerInterest[source + 1];
	interest.centre = packet->centre;
	interest.radius = packet->radius;
}

void DistributedGameServer::DistributedGameServerManager::BroadcastSnapshot(bool deltaFrame) {
	std::vector<GameObject*>::const_iterator first;
	std::vector<GameObject*>::const_iterator last;

	mServerWorldManager->GetGameWorld()->GetObjectIterators(first, last);

	// Peer list and their declared interest, resolved once per snapshot rather than
	// per object: the set only changes when a client sends a new declaration.
	//
	// A peer with no declaration, or one with radius <= 0, gets EVERYTHING. That is
	// what a client which does not implement interest receives, and it is also what
	// the other servers receive - they do not consume snapshots at all, but excluding
	// them would mean deciding which peers are servers, and getting that wrong would
	// silently starve a real client.
	struct SnapshotTarget {
		int peer;
		const PeerInterest* interest;   // nullptr = send everything
	};
	std::vector<SnapshotTarget> targets;
	bool anyFiltered = false;
	// Peers that asked for nothing at all. Counted per object below so the suppressed
	// total reflects the traffic they would otherwise have received.
	int silencedPeers = 0;
	for (int peer : mDistributedPacketSenderServer->GetConnectedPeers()) {
		const auto found = mPeerInterest.find(peer);
		if (found != mPeerInterest.end() && found->second.radius < 0.0f) {
			// Wants nothing at all - a peer server. Counted as suppressed rather than
			// skipped silently, because the volume it represents is the point.
			anyFiltered = true;
			++silencedPeers;
			continue;
		}
		const PeerInterest* interest =
			(found != mPeerInterest.end() && found->second.radius > 0.0f) ? &found->second : nullptr;
		targets.push_back({ peer, interest });
		anyFiltered = anyFiltered || (interest != nullptr);
	}

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

				mSnapshotsSuppressed += silencedPeers;

				if (!anyFiltered) {
					// Nobody is filtering, so one broadcast is cheaper than a send per
					// peer. This is exactly the old behaviour.
					mDistributedPacketSenderServer->SendGlobalPacket(*newPacket);
					mSnapshotsSent += static_cast<long long>(targets.size());
				}
				else {
					// The packet is built ONCE and sent to the peers that want it.
					// Rebuilding it per peer would make the CPU cost scale with
					// clients as well as objects, trading one scaling problem for
					// another.
					const Maths::Vector3 position = (*i)->GetTransform().GetPosition();
					for (const SnapshotTarget& target : targets) {
						if (target.interest != nullptr) {
							// Distance on XZ only, matching every other spatial test
							// here: regions, the halo band and the broadphase are all
							// two-dimensional, so an interest SPHERE would be the one
							// place that disagreed with them.
							const float dx = position.x - target.interest->centre.x;
							const float dz = position.z - target.interest->centre.z;
							const float radius = target.interest->radius;
							if ((dx * dx + dz * dz) > (radius * radius)) {
								++mSnapshotsSuppressed;
								continue;
							}
						}
						mDistributedPacketSenderServer->SendPacketToPeer(target.peer, *newPacket);
						++mSnapshotsSent;
					}
				}
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
	if (packet == nullptr || mServerWorldManager == nullptr) {
		return;
	}
	mServerWorldManager->HandleTransitionHandshakeReceived(packet);
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

	// Kept for the packet-sender bound, which the registry recomputes once it is
	// complete. The arrays further down this packet are the legacy 20-server path and
	// are only still read so a run works if the registry has not arrived yet.
	mExpectedClientCount = packet->clientsToConnect;

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
	// Collected FIRST, then acted on. HandleOutgoingObject now tears the object down
	// rather than merely deactivating it, and teardown erases the object's
	// NetworkObject from the very vector being walked here - so iterating it directly
	// while releasing objects invalidates the iterators mid-loop. The transition set
	// is usually tiny, so copying it is free.
	std::vector<NetworkObject*> transitioning;
	for (auto& networkObj : *mNetworkObjects) {
		if (networkObj->GetIsActualPosOutOfServer()) {
			transitioning.push_back(networkObj);
		}
	}

	// RATE LIMITED, and this is not an optimisation.
	//
	// Ordinary border traffic is a handful of objects a tick. A border MOVE is not: a
	// repartition can put thousands of objects outside their owner's region at once,
	// and every one of them is a reliable packet. Sending 6,000 in a single tick
	// overwhelmed the link - the sender had already scheduled each release, so
	// everything that failed to arrive was lost outright. Measured on a 6,000-object
	// cluster: 6,078 transfers sent, 1,343 accounted for, 4,735 objects gone.
	//
	// Capping turns a border move into a migration spread over several ticks. Nothing
	// else has to change: an object left over stays outside its region, so the border
	// check re-flags it next tick, and IsReleasePending stops the ones already in
	// flight being sent twice. Ownership stays continuous throughout because each
	// object is still transferred atomically at its own agreed tick.
	//
	// A deliberate consequence: a large repartition takes
	// ceil(objects / MAX_HANDOFFS_PER_TICK) ticks to complete, and that is the honest
	// cost of moving a border. It is reported rather than hidden.
	constexpr size_t MAX_HANDOFFS_PER_TICK = 64;
	if (transitioning.size() > MAX_HANDOFFS_PER_TICK) {
		// Sorted by object id first, so which objects go in this batch does not depend
		// on the order mNetworkObjects happens to be in - that order changes as objects
		// are torn down and rebuilt, and a migration that picked a different batch each
		// run would not be reproducible.
		std::sort(transitioning.begin(), transitioning.end(),
			[](NetworkObject* l, NetworkObject* r) {
				return l->GetNetworkID() < r->GetNetworkID();
			});
		transitioning.resize(MAX_HANDOFFS_PER_TICK);
	}

	for (auto* networkObj : transitioning) {
		// Release the object ONLY once the packet is actually on a link to the new
		// owner. The transition flag is left set on failure, so the next tick
		// retries rather than the object being lost to a link that was not up yet.
		StartSimulatingObjectPacket sentPacket;
		if (!SendFinishTransactionPacket(*networkObj, sentPacket)) {
			continue;
		}
		// Logged AFTER the send, not before it. The transition flag stays set on
		// failure so the next tick retries, which means a pre-send trace prints once
		// per pending object per tick for as long as the link is down - 235,578 lines
		// in one run against 400 handoffs actually made. One line per handoff that
		// really happened is the useful form.
		std::cout << "Handoff: object " << networkObj->GetNetworkID()
			<< " -> server " << networkObj->GetNewServerID() << "\n";
		mServerWorldManager->RecordHandoffSent();
		// Custody starts here, not at release. The object is released on its normal
		// tick below; this record is what lets an unacknowledged transfer be resent
		// and, failing that, reclaimed.
		//
		// Deferred handoffs (--handoff-delay-ticks) leave sentPacket untouched, and
		// deliberately so: that flag is fault injection whose purpose is to widen the
		// ownership gap and lose objects, so custody must not silently repair it.
		// Guarding on the id also stops an unackable record under objectID -1, which
		// would eventually reclaim a default-constructed packet.
		if (sentPacket.objectID >= 0) {
			// transitioning.size() - not 1 - so custody's retry deadline (see
			// SetCustodyConfig / PendingTransfer::batchSizeAtSend) knows this transfer
			// went out as part of a batch: a receiver handed N handoffs in the same tick
			// applies and acks them one at a time, so it genuinely needs about N times as
			// long as a lone transfer would.
			mServerWorldManager->RecordPendingTransfer(sentPacket,
				static_cast<int>(transitioning.size()));
		}
		// Read before the release: HandleOutgoingObject destroys the object that owns
		// this NetworkObject, so nothing may be read back off it afterwards.
		const int networkID = networkObj->GetNetworkID();
		const int newOwner = networkObj->GetNewServerID();
		networkObj->HandleTransitionComplete();
		// Scheduled for the tick the RECEIVER installs it on, not released now.
		// Releasing on send left the object owned by nobody for the whole lookahead.
		mServerWorldManager->ScheduleOutgoingObject(networkID, newOwner);
	}
}

// Peer links are keyed by SERVER ID, never by array index. StartDistributedGameServerPacket
// carries two differently-indexed array families and connectedServerIDs[] is what maps
// registration order back to real ids; using the index instead silently sends to a link
// that does not exist. Solved once, here.
// Unreliable counterpart of SendPacketToServer, for state that is superseded every
// tick. A halo update is the neighbour's view of an object's current position; a
// retransmitted one is a stale position that has already been replaced, so paying for
// reliability would deliver something the receiver must then discard. Snapshots are
// unreliable for exactly this reason. Anything ONE-SHOT - handoff, spawn, despawn -
// must stay reliable.
bool DistributedGameServer::DistributedGameServerManager::SendUnreliablePacketToServer(
	int targetServerID, GamePacket& packet) const {
	for (auto* connection : mDistributedPhysicsClients) {
		if (connection->serverID == targetServerID && connection->client != nullptr) {
			connection->client->SendPacket(packet);
			return true;
		}
	}
	return false;
}

bool DistributedGameServer::DistributedGameServerManager::SendPacketToServer(int targetServerID,
	GamePacket& packet) const {
	for (const auto* connection : mDistributedPhysicsClients) {
		if (connection->serverID == targetServerID && connection->client != nullptr) {
			// Propagated, not assumed. A handoff releases the object on the strength of
			// this returning true, so reporting success for a packet ENet refused
			// loses the object outright.
			return connection->client->SendReliablePacket(packet);
		}
	}
	return false;
}

// Deliberately separate from SendPacketToServer's return value. That returns false for
// two very different things: no peer link at all, and ENet refusing an otherwise-valid
// send because the peer's outgoing reliable queue is full. The second is backpressure
// under overload - exactly the condition custody must not read as "the peer died" - so
// custody asks this instead. See NCL::Distributed::DecideCustody.
bool DistributedGameServer::DistributedGameServerManager::HasPeerLink(int targetServerID) const {
	for (const auto* connection : mDistributedPhysicsClients) {
		if (connection->serverID == targetServerID && connection->client != nullptr) {
			return true;
		}
	}
	return false;
}

// Powers of ten rather than a timer: a link that comes back goes quiet on its own,
// and one that stays down still leaves a record of how bad it got without writing a
// line per object per tick.
bool DistributedGameServer::DistributedGameServerManager::ShouldLogMissingLink(
	int targetServerID) const {
	const long long count = ++mMissingLinkFailures[targetServerID];
	long long threshold = 1;
	while (threshold < count) {
		threshold *= 10;
	}
	return threshold == count;
}

bool DistributedGameServer::DistributedGameServerManager::SendFinishTransactionPacket(NetworkObject& obj,
	StartSimulatingObjectPacket& outSent) const {
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
	outSent = packet;
	if (!SendPacketToServer(packet.newOwnerServerID, packet)) {
		// Loud, and the caller keeps the object. There is no broadcast to fall back
		// on now, so releasing it here would destroy it outright.
		if (ShouldLogMissingLink(packet.newOwnerServerID)) {
			std::cout << "ERROR: no peer link to server " << packet.newOwnerServerID
				<< " for handoff of object " << packet.objectID
				<< " - object retained, will retry next tick.\n";
		}
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

		// Still a broadcast, but for a different reason than before. Clients need it
		// to build a replica. Peers no longer build anything - they take only the
		// owner id from it, so that a command aimed at this object before the client
		// has learned its owner can be forwarded rather than rejected.
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

	// Archetype and position are deliberately unused here: a non-owner builds nothing,
	// so it needs neither. They are still on the wire because the same broadcast is what
	// CLIENTS use to build their replica.
	worldManager->RecordRemoteSpawn(packet->objectID, packet->ownerServerID);
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

// Sends every neighbour the objects this server owns that are near its region, so
// those objects can take part in contacts computed on the far side of the border.
//
// Per tick, not per event: a shadow is a continuously tracked position. That is also
// why it goes out unreliably - see SendUnreliablePacketToServer.
void DistributedGameServer::DistributedGameServerManager::PublishHaloBand() {
	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr || worldManager->GetHaloWidth() <= 0.0f) {
		return;
	}

	// Once per tick, not once per call. The batching comment below states the intended
	// pattern as one packet per neighbour per tick, but the caller runs every loop
	// iteration, and a loop iteration is not a tick:
	//
	//  - in realtime mode the loop spins at ~1 kHz against a 120 Hz substep, so roughly
	//    seven publishes in eight re-send an unchanged state under the same senderTick.
	//    The receiver schedules each at senderTick + lookahead and the later copies
	//    overwrite the earlier ones with identical values, so they are pure bandwidth.
	//  - the drain phase calls this too (ServerStarter.cpp) but never steps the world,
	//    so mTickCounter is frozen for its whole duration and every iteration
	//    republished the entire band. On the lookahead sweeps that flood was about 97%
	//    of haloObjSent, which is why E8's bandwidth figures had to be retracted.
	//
	// A paced run already does exactly one iteration per tick, so this changes nothing
	// there and leaves the E2 and E5 measurements valid.
	const uint64_t currentTick = worldManager->GetTickCounter();
	if (currentTick == mLastHaloPublishTick) {
		return;
	}
	mLastHaloPublishTick = currentTick;

	std::vector<ServerWorldManager::HaloPublication> publications;
	worldManager->CollectHaloPublications(publications);
	if (publications.empty()) {
		return;
	}

	// Sorted by (target, object) already, so one pass fills a batch per target and
	// flushes on the boundary. Batching matters: one packet per object per neighbour
	// per tick is the traffic pattern this increment has to avoid being dismissed for.
	const int senderTick = static_cast<int>(currentTick);
	int currentTarget = -1;
	HaloUpdatePacket batch(mGameServerID, senderTick);

	auto flush = [&]() {
		if (batch.entryCount == 0 || currentTarget < 0) {
			return;
		}
		// Reliable when the run has to be reproducible, unreliable otherwise.
		//
		// Unreliable is the natural choice - a halo update is superseded next tick,
		// exactly like a snapshot - and it is what a production deployment wants. But
		// a dropped update leaves the shadow extrapolating from an older sample, and
		// which packets drop is not the same from run to run: two otherwise identical
		// uniform runs differed by one received update and by fourteen contacts.
		// Reliable delivery costs bandwidth and can deliver a state that is already
		// stale, which is why it is not the default; it is what makes a measurement
		// run repeatable.
		const bool delivered = mHaloReliable
			? SendPacketToServer(currentTarget, batch)
			: SendUnreliablePacketToServer(currentTarget, batch);
		if (delivered) {
			++mHaloUpdatesSent;
			mHaloObjectsSent += batch.entryCount;
		}
		batch = HaloUpdatePacket(mGameServerID, senderTick);
	};

	for (const auto& publication : publications) {
		if (publication.targetServerID != currentTarget) {
			flush();
			currentTarget = publication.targetServerID;
		}

		HaloObjectState state;
		if (!worldManager->TryGetHaloState(publication.objectID, state)) {
			continue;   // Handed off or destroyed since CollectHaloPublications ran.
		}
		if (!batch.TryAdd(state)) {
			flush();
			batch.TryAdd(state);
		}
	}
	flush();
}

void DistributedGameServer::DistributedGameServerManager::HandleRepartitionPacket(
	DistributedRepartitionPacket* packet) {
	if (packet == nullptr) {
		return;
	}
	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr) {
		return;
	}

	// Accumulated by effective tick, because a partition arrives in PAGES. Adopting a
	// partial one would leave this server disagreeing with its peers about where the
	// borders are, which is the one thing a repartition must never do - so nothing is
	// scheduled until every region has arrived.
	auto& assembling = mAssemblingPartitions[packet->effectiveTick];

	// regionCount, not the page capacity: the packet was sized to the regions actually
	// used, so anything past it was never sent and reading it would run off the end of
	// the received buffer.
	const int count = std::min(packet->regionCount,
		DistributedRepartitionPacket::MAX_REGIONS_PER_PAGE);
	for (int i = 0; i < count; ++i) {
		const RegionBoundsWire& wire = packet->regions[i];
		// Keyed by server id, so a page delivered twice cannot produce a duplicate
		// region and a partition with a hole is impossible to mistake for a full one.
		assembling[wire.serverID] = NCL::Interaction::RegionBounds{
			wire.serverID, wire.minX, wire.maxX, wire.minZ, wire.maxZ };
	}

	if (static_cast<int>(assembling.size()) < packet->totalRegionCount) {
		std::cout << "Repartition page: " << assembling.size() << " of "
			<< packet->totalRegionCount << " regions for tick "
			<< packet->effectiveTick << "\n";
		return;
	}

	ServerWorldManager::PendingPartition partition;
	partition.effectiveTick = packet->effectiveTick;
	partition.regions.reserve(assembling.size());
	for (const auto& entry : assembling) {
		partition.regions.push_back(entry.second);
	}
	mAssemblingPartitions.erase(packet->effectiveTick);

	std::cout << "Repartition complete: " << partition.regions.size()
		<< " regions, effective at tick " << packet->effectiveTick << "\n";
	worldManager->SchedulePartitionChange(partition);
}

// One page of the instance's server registry.
//
// This replaces the fixed 20-entry arrays that used to ride inside the start packet
// and capped an instance at 20 servers. Pages are accumulated until the registry is
// complete; nothing is acted on before that, so a page arriving late or out of order
// costs nothing.
void DistributedGameServer::DistributedGameServerManager::HandleServerRegistryPacket(
	DistributedServerRegistryPacket* packet) {
	if (packet == nullptr || packet->gameInstanceID != mGameInstanceID) {
		return;
	}

	const int count = std::min(packet->entryCount,
		DistributedServerRegistryPacket::MAX_ENTRIES_PER_PAGE);
	for (int i = 0; i < count; ++i) {
		// Keyed by server id: a page delivered twice overwrites rather than appending,
		// so the completeness test below cannot be satisfied by duplicates.
		mServerRegistry[packet->entries[i].serverID] = packet->entries[i];
	}

	mRegistryTotalServerCount = packet->totalServerCount;
	if (static_cast<int>(mServerRegistry.size()) < packet->totalServerCount) {
		std::cout << "Server registry: " << mServerRegistry.size() << " of "
			<< packet->totalServerCount << " servers\n";
		return;
	}

	std::cout << "Server registry complete: " << mServerRegistry.size() << " servers\n";
	ApplyServerRegistry();
}

// Builds the border map and the peer links from a COMPLETE registry.
//
// Idempotent: the registry is rebroadcast whenever another server registers, so this
// runs several times during bootstrap and must only ever add what is missing.
void DistributedGameServer::DistributedGameServerManager::ApplyServerRegistry() {
	for (const auto& entry : mServerRegistry) {
		if (mPhysicsServerBorderMap.contains(entry.first)) {
			continue;
		}
		auto* border = new PhysicsServerBorderData();
		border->minXVal = entry.second.minX;
		border->maxXVal = entry.second.maxX;
		border->minZVal = entry.second.minZ;
		border->maxZVal = entry.second.maxZ;
		mPhysicsServerBorderMap.insert(std::make_pair(entry.first, border));
		std::cout << "Registry border for server " << entry.first
			<< " x " << border->minXVal << ".." << border->maxXVal
			<< " z " << border->minZVal << ".." << border->maxZVal << "\n";
	}

	// The packet sender has to expect every peer plus every client. Recomputed here
	// rather than taken from the start packet, since the registry is the authority on
	// how many servers the instance has.
	if (mDistributedPacketSenderServer != nullptr && mRegistryTotalServerCount > 0) {
		mDistributedPacketSenderServer->SetMaxClients(
			(mRegistryTotalServerCount - 1) + mExpectedClientCount);
	}

	for (const auto& entry : mServerRegistry) {
		if (entry.first == mGameServerID) {
			continue;
		}
		// A server that has not registered with the manager yet has a region but no
		// address. Its borders are already usable; the link is made when a later
		// registry broadcast carries its address.
		const std::string peerIP(entry.second.ip);
		if (peerIP.empty() || entry.second.port == 0) {
			continue;
		}
		// Our own entry, matched by address and port rather than id, because the
		// registry lists this server exactly as it lists any other.
		if (peerIP == DistributedUtils::GetMachineIPV4Address()
			&& entry.second.port == mPacketSenderServerPort) {
			continue;
		}

		bool alreadyLinked = false;
		for (const auto* connection : mDistributedPhysicsClients) {
			if (connection->serverID == entry.first) {
				alreadyLinked = true;
				break;
			}
		}
		if (alreadyLinked) {
			continue;
		}

		const std::vector<char> octets = IpToCharArray(peerIP);
		if (octets.size() < 4) {
			continue;
		}
		if (auto* connection = ConnectServerToAnotherGameServer(
			octets[0], octets[1], octets[2], octets[3], entry.second.port, entry.first)) {
			std::cout << "Linked to server " << entry.first << " from registry\n";
			mDistributedPhysicsClients.push_back(connection);
		}
		else {
			std::cout << "Failed to link to server " << entry.first << " from registry\n";
		}
	}
}

void DistributedGameServer::DistributedGameServerManager::HandleHaloUpdatePacket(
	HaloUpdatePacket* packet) {
	if (packet == nullptr) {
		return;
	}
	// Our own update coming back to us would mean an object is being shadowed by its
	// own owner, which is a second copy of something we already simulate.
	if (packet->senderServerID == mGameServerID) {
		return;
	}

	++mHaloUpdatesReceived;
	mHaloObjectsReceived += packet->entryCount;

	ServerWorldManager* worldManager = GetServerWorldManager();
	if (worldManager == nullptr) {
		return;
	}

	// entryCount, not MAX_ENTRIES: the packet was sized to the entries actually used,
	// so anything past entryCount was never sent and reading it would be reading off
	// the end of the received buffer.
	const int count = std::min(packet->entryCount, HaloUpdatePacket::MAX_ENTRIES);
	for (int i = 0; i < count; ++i) {
		worldManager->ScheduleHaloUpdate(packet->entries[i], packet->senderTick,
			packet->senderServerID);
	}
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
		// Registered on BOTH directions, like handoff. Which link a type travels on
		// depends on where its handler lives, and a type sent down a direction with no
		// handler is dropped by ENet in complete silence - no error, no counter.
		client->RegisterPacketHandler(BasicNetworkMessages::DistributedHaloUpdate, this);
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
