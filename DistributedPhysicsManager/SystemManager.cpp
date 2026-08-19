#include "SystemManager.h"

#include <algorithm>
#include <iostream>

#include "DistributedPhysicsManagerServer.h"
#include "GameServer.h"
#include "NetworkBase.h"
#include "NetworkObject.h"
#include "Profiler.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/DistributedPhysicsServerDto.h"
#include "ServerWorldManager.h"
#include "DistributedSystemCommonFiles/DistributedUtils.h"

using namespace NCL;

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
	mSystemManagerPort = port;
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
	mDistributedPhysicsManagerServer->RegisterPacketHandler(BasicNetworkMessages::DistributedServerLoadReport, this);
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
	case BasicNetworkMessages::DistributedServerLoadReport: {
		HandleServerLoadReport(static_cast<NCL::CSC8503::DistributedServerLoadReportPacket*>(payload));
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

	// RELIABLE, not SendGlobalPacket. This is a one-shot bootstrap message with no
	// retry anywhere: a server that misses it never starts its world at all, reports
	// game=0 forever and produces no metrics. Sent unreliably it was dropped for
	// roughly one recipient in four - which is why 4-server runs lost exactly one
	// server, and which one varied. Snapshots are correctly unreliable because they
	// are superseded 60 times a second; this is not.
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(state);

	// Sent immediately after the start signal, not on a timer. The effective tick is
	// ABSOLUTE and every server aligns its tick 0 to a shared epoch, so sending it
	// early is what makes the switch land on the same simulated tick everywhere,
	// regardless of when each server happens to receive it.
	if (mForcedRepartitionTick > 0 && !mForcedRepartitionX.empty()) {
		SendRepartitionPacket(gameInstanceID, mForcedRepartitionTick, mForcedRepartitionX);
	}
}

// One server's load report. Held until the round is complete.
void NCL::DistributedManager::SystemManager::HandleServerLoadReport(
	NCL::CSC8503::DistributedServerLoadReportPacket* packet) {
	if (packet == nullptr) {
		return;
	}

	ServerLoadReport report;
	report.serverID = packet->serverID;
	report.contacts = packet->contacts;
	report.ownedObjects = packet->ownedObjects;
	report.minX = packet->minX;
	report.maxX = packet->maxX;
	for (int i = 0; i < NCL::CSC8503::DistributedServerLoadReportPacket::LOAD_BUCKETS; ++i) {
		report.buckets[i] = packet->bucketContacts[i];
	}
	mLoadReports[packet->tick][packet->serverID] = report;

	GameInstance* instance = (mDistributedPhysicsManagerServer != nullptr)
		? mDistributedPhysicsManagerServer->GetGameInstance(packet->gameInstanceID)
		: nullptr;
	if (instance == nullptr) {
		return;
	}

	const size_t expected = instance->GetServerBorderMap().size();
	if (mLoadReports[packet->tick].size() < expected) {
		return;   // Round not complete; a partial one would balance against a hole.
	}

	RunRebalancePolicy(packet->gameInstanceID, packet->tick);

	// Everything up to and including this round is spent. Older rounds can never
	// complete now - the servers have moved past them - so they would otherwise sit in
	// the map for the life of the process.
	mLoadReports.erase(mLoadReports.begin(), mLoadReports.upper_bound(packet->tick));
}

// Moves each interior boundary towards the heavier of the two servers it separates.
//
// A diffusive rule, not a global optimum: each boundary is corrected by a fraction of
// its own local imbalance, and repeated rounds converge. That is deliberately the
// simplest policy that works, because the risky part of this increment was the
// mechanism - moving a border without losing an object - and a crude policy on a safe
// mechanism is a result, where a perfect policy on an unsafe one is worthless.
//
// It balances CONTACTS, not object counts. The best partition found by hand for the
// shuttle workload held 100 objects against 300 and still had near-equal wall clock,
// because its contact counts were near-equal: contact cost scales with local density,
// which an object count cannot see.
void NCL::DistributedManager::SystemManager::RunRebalancePolicy(int gameInstanceID,
	long long tick) {
	const auto round = mLoadReports.find(tick);
	if (round == mLoadReports.end()) {
		return;
	}

	// Nothing is decided until the previous move has taken effect AND its bulk handoff
	// has settled.
	//
	// This is not politeness, it is correctness. A border move can transfer hundreds of
	// objects, each released at senderTick + lookahead. Deciding again before those
	// have landed means measuring a partition that does not exist yet, and the second
	// decision is made from load that is still in flight. The first version of this
	// policy did exactly that on a 300-tick interval against a 300-tick lookahead: the
	// boundary swung -75, -67, -135, -10 over four rounds and 396 of 400 objects were
	// lost in the churn.
	constexpr long long SETTLE_TICKS = 300;
	if (mLastRepartitionEffectiveTick >= 0
		&& tick < mLastRepartitionEffectiveTick + SETTLE_TICKS) {
		return;
	}

	// Ordered by server id, and the partition is a 1-D split in the same order, so
	// entry i and entry i+1 are neighbours sharing one boundary.
	std::vector<ServerLoadReport> reports;
	reports.reserve(round->second.size());
	for (const auto& entry : round->second) {
		reports.push_back(entry.second);
	}
	if (reports.size() < 2) {
		return;   // Nothing to balance.
	}

	long long totalContacts = 0;
	for (const ServerLoadReport& report : reports) {
		totalContacts += report.contacts;
	}
	if (totalContacts <= 0) {
		return;   // Nothing happened this round; no evidence to act on.
	}

	std::vector<double> boundaries;
	bool anyMoved = false;

	for (size_t i = 0; i + 1 < reports.size(); ++i) {
		const ServerLoadReport& left = reports[i];
		const ServerLoadReport& right = reports[i + 1];
		const double boundary = static_cast<double>(left.maxX);
		const double leftWidth = boundary - static_cast<double>(left.minX);
		const double rightWidth = static_cast<double>(right.maxX) - boundary;

		if (leftWidth <= 0.0 || rightWidth <= 0.0) {
			boundaries.push_back(boundary);
			continue;
		}

		const double pairLoad = static_cast<double>(left.contacts + right.contacts);
		if (pairLoad <= 0.0) {
			boundaries.push_back(boundary);
			continue;
		}

		const double imbalance =
			(static_cast<double>(left.contacts) - static_cast<double>(right.contacts)) / pairLoad;
		if (std::abs(imbalance) < mRebalanceThreshold) {
			boundaries.push_back(boundary);   // Inside the dead band; leave it alone.
			continue;
		}

		// Build the pair's load profile along X from both servers' histograms, then
		// find the X that splits it in half.
		//
		// A single total per server cannot do this. A cluster sitting entirely inside
		// one region looks the same whether it is at that region's left edge or its
		// right, so a policy working from totals can only guess which way to move the
		// border - and guessing wrong walks it straight past the cluster. That is what
		// happened: the border was driven until one server held all 4,000 objects and
		// the other held none, having only swapped which server was overloaded.
		constexpr int BUCKETS = NCL::CSC8503::DistributedServerLoadReportPacket::LOAD_BUCKETS;
		struct Slice { double from; double to; double load; };
		std::vector<Slice> profile;
		profile.reserve(BUCKETS * 2);

		const double leftBucketWidth = leftWidth / BUCKETS;
		for (int b = 0; b < BUCKETS; ++b) {
			const double from = static_cast<double>(left.minX) + leftBucketWidth * b;
			profile.push_back({ from, from + leftBucketWidth, static_cast<double>(left.buckets[b]) });
		}
		const double rightBucketWidth = rightWidth / BUCKETS;
		for (int b = 0; b < BUCKETS; ++b) {
			const double from = boundary + rightBucketWidth * b;
			profile.push_back({ from, from + rightBucketWidth, static_cast<double>(right.buckets[b]) });
		}

		double profileTotal = 0.0;
		for (const Slice& slice : profile) {
			profileTotal += slice.load;
		}
		if (profileTotal <= 0.0) {
			boundaries.push_back(boundary);
			continue;
		}

		// Walk the profile until half the load is behind us, interpolating inside the
		// slice that straddles the halfway point so the answer is finer than a bucket.
		const double half = profileTotal * 0.5;
		double running = 0.0;
		double target = boundary;
		for (const Slice& slice : profile) {
			if (running + slice.load >= half) {
				const double needed = half - running;
				const double fraction = (slice.load > 0.0) ? (needed / slice.load) : 0.0;
				target = slice.from + (slice.to - slice.from) * fraction;
				break;
			}
			running += slice.load;
		}

		double moved = boundary + mRebalanceAlpha * (target - boundary);

		// Still clamped per round. The profile is a snapshot of a world that keeps
		// moving, so a large single step lands on where the load WAS.
		constexpr double MAX_STEP_FRACTION = 0.15;
		const double maxStep = (leftWidth + rightWidth) * MAX_STEP_FRACTION;
		moved = std::max(boundary - maxStep, std::min(boundary + maxStep, moved));

		// Never so close to a neighbour that a region becomes degenerate. An EMPTY
		// region is legal and the policy must be allowed to produce one; a zero-width
		// region is not, because every point in the world must belong to someone.
		constexpr double MIN_REGION_WIDTH = 1.0;
		const double lowerLimit = static_cast<double>(left.minX) + MIN_REGION_WIDTH;
		const double upperLimit = static_cast<double>(right.maxX) - MIN_REGION_WIDTH;
		if (lowerLimit < upperLimit) {
			moved = std::max(lowerLimit, std::min(upperLimit, moved));
		}
		else {
			moved = boundary;
		}

		if (std::abs(moved - boundary) > 1e-3) {
			anyMoved = true;
		}
		boundaries.push_back(moved);
	}

	if (!anyMoved) {
		return;
	}

	// Far enough ahead that every server has the packet before the tick arrives. The
	// same margin the handoff lookahead uses, for the same reason.
	constexpr long long REPARTITION_MARGIN_TICKS = 300;
	const long long effectiveTick = tick + REPARTITION_MARGIN_TICKS;
	mLastRebalanceTick = tick;
	mLastRepartitionEffectiveTick = effectiveTick;

	std::cout << "Rebalance at tick " << tick << " (effective " << effectiveTick << "): loads";
	for (const ServerLoadReport& report : reports) {
		std::cout << " [" << report.serverID << " contacts=" << report.contacts
			<< " objs=" << report.ownedObjects << "]";
	}
	std::cout << " -> boundaries";
	for (double boundary : boundaries) {
		std::cout << " " << boundary;
	}
	std::cout << "\n";

	SendRepartitionPacket(gameInstanceID, effectiveTick, boundaries);
}

void NCL::DistributedManager::SystemManager::SendRepartitionPacket(int gameInstanceID,
	long long effectiveTick, const std::vector<double>& interiorX) {
	// From the manager server, which is where CreateNewGameInstance actually puts
	// them; mCreatedGameInstances is declared but never populated.
	GameInstance* instance = (mDistributedPhysicsManagerServer != nullptr)
		? mDistributedPhysicsManagerServer->GetGameInstance(gameInstanceID)
		: nullptr;
	if (instance == nullptr) {
		std::cout << "ERROR: repartition requested for unknown game instance "
			<< gameInstanceID << "\n";
		return;
	}

	double worldMinX = 0.0, worldMaxX = 0.0, worldMinZ = 0.0, worldMaxZ = 0.0;
	instance->GetWorldBounds(worldMinX, worldMaxX, worldMinZ, worldMaxZ);

	// Server ids taken from the EXISTING partition, in ascending order, so slice i
	// goes to the same server whatever order the map happens to be in.
	std::vector<int> serverIDs;
	for (const auto& entry : instance->GetServerBorderMap()) {
		serverIDs.push_back(entry.first);
	}
	std::sort(serverIDs.begin(), serverIDs.end());

	if (serverIDs.size() != interiorX.size() + 1) {
		std::cout << "ERROR: --repartition-x gives " << interiorX.size()
			<< " interior boundaries, which makes " << (interiorX.size() + 1)
			<< " slices, but this instance has " << serverIDs.size() << " servers.\n";
		return;
	}

	std::vector<double> sortedX = interiorX;
	std::sort(sortedX.begin(), sortedX.end());

	// Paged, like the registry: a fixed bound here would cap the server count exactly
	// as the bootstrap arrays used to. A receiver adopts nothing until it holds all
	// totalRegionCount regions, so a partial partition is never applied.
	const int totalRegions = static_cast<int>(serverIDs.size());
	DistributedRepartitionPacket page(effectiveTick, totalRegions);
	int pagesSent = 0;

	auto flush = [&]() {
		if (page.regionCount == 0) {
			return;
		}
		// Reliable and one-shot: a server that misses a page stays on the old
		// partition while everyone else moves, and every ownership question is then
		// answered differently there than anywhere else.
		mDistributedPhysicsManagerServer->SendGlobalReliablePacket(page);
		++pagesSent;
		page = DistributedRepartitionPacket(effectiveTick, totalRegions);
	};

	for (size_t i = 0; i < serverIDs.size(); ++i) {
		RegionBoundsWire region;
		region.serverID = serverIDs[i];
		region.minX = static_cast<float>((i == 0) ? worldMinX : sortedX[i - 1]);
		region.maxX = static_cast<float>((i == serverIDs.size() - 1) ? worldMaxX : sortedX[i]);
		// Slices span the whole Z extent. A 1-D split is all the forced-repartition
		// flag needs to express, and it is also what the first policy will produce.
		region.minZ = static_cast<float>(worldMinZ);
		region.maxZ = static_cast<float>(worldMaxZ);
		if (!page.TryAddRegion(region)) {
			flush();
			page.TryAddRegion(region);
		}
	}
	flush();

	std::cout << "Broadcasting repartition: " << totalRegions << " regions in "
		<< pagesSent << " page(s), effective at tick " << effectiveTick << "\n";
}

void DistributedManager::SystemManager::
SendDistributedPhysicsServerInfoToClients(const std::string& ip, const int serverID, const int port, const std::string& borderStr) const {
	DistributedClientConnectToPhysicsServerPacket packet(port, serverID, ip, borderStr);
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(packet);
}

void DistributedManager::SystemManager::SendStartDataToPhysicsServer(int gameInstanceID, int physicsServerID) const {
	std::vector<int> serverPorts;
	std::vector<std::string> serverIps;
	// Registration order, not id order - so the id has to travel with each entry.
	std::vector<int> connectedServerIds;

	auto* gameInstance = mDistributedPhysicsManagerServer->GetGameInstance(gameInstanceID);

	for (const auto& createdServer : mDistributedPhysicsServers) {
		serverIps.push_back(createdServer->GetServerIPAddress());
		serverPorts.push_back(createdServer->GetDataSenderPort());
		connectedServerIds.push_back(createdServer->GetServerID());
	}
	auto& physicsServersBorderStrMap = gameInstance->GetServerBorderStrMap();
	StartDistributedGameServerPacket packet(mSystemManagerPort, gameInstanceID, mMaxClientCount, gameInstance->GetObjectsPerPlayer(), serverPorts, serverIps, connectedServerIds, physicsServersBorderStrMap);
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(packet);

	// The registry the receivers actually use. The arrays inside the packet above are
	// retained only so an unmodified role still boots; they cap an instance at 20
	// servers and are ignored by any receiver that has assembled a full registry.
	SendServerRegistry(gameInstanceID);
}

// The instance's server registry, in pages.
//
// This is what removes the 20-server ceiling. The registry used to ride inside
// StartDistributedGameServerPacket as five fixed 20-entry arrays - the largest being
// char borders[20][256] - which made that one message about 6 KB and capped an
// instance at 20 servers however much hardware was available. Widening the arrays
// only moves the problem: sized for 200 servers the same packet is over 50 KB, sent
// to everyone, whatever the instance's actual size.
//
// Reliable, because this is one-shot bootstrap state with no retry: a server missing
// a single page never assembles a complete registry and never builds its world.
void DistributedManager::SystemManager::SendServerRegistry(int gameInstanceID) const {
	GameInstance* instance = (mDistributedPhysicsManagerServer != nullptr)
		? mDistributedPhysicsManagerServer->GetGameInstance(gameInstanceID)
		: nullptr;
	if (instance == nullptr) {
		return;
	}

	const auto& borderMap = instance->GetServerBorderMap();

	// Registration data is keyed by server id here, unlike the legacy arrays, so a
	// receiver never has to reconcile two differently-indexed families.
	// Non-const pointers: the accessors on DistributedPhysicsServerData are not const.
	std::map<int, DistributedPhysicsServerData*> registered;
	for (auto* server : mDistributedPhysicsServers) {
		if (server->GetGameInstanceID() == gameInstanceID) {
			registered[server->GetServerID()] = server;
		}
	}

	const int totalServerCount = static_cast<int>(borderMap.size());
	DistributedServerRegistryPacket page(gameInstanceID, totalServerCount);
	int pagesSent = 0;

	auto flush = [&]() {
		if (page.entryCount == 0) {
			return;
		}
		mDistributedPhysicsManagerServer->SendGlobalReliablePacket(page);
		++pagesSent;
		page = DistributedServerRegistryPacket(gameInstanceID, totalServerCount);
	};

	for (const auto& border : borderMap) {
		if (border.second == nullptr) {
			continue;
		}

		ServerRegistryEntry entry{};
		entry.serverID = border.first;
		entry.minX = static_cast<float>(border.second->minX);
		entry.maxX = static_cast<float>(border.second->maxX);
		entry.minZ = static_cast<float>(border.second->minZ);
		entry.maxZ = static_cast<float>(border.second->maxZ);

		// A server that has not registered yet still gets a region entry, with an
		// empty address: peers need its BORDERS to answer ownership questions long
		// before they need a link to it, and withholding the region until it connects
		// would leave holes in the partition.
		const auto found = registered.find(border.first);
		if (found != registered.end()) {
			entry.port = found->second->GetDataSenderPort();
			CopyToPacketField(entry.ip, found->second->GetServerIPAddress());
		}
		else {
			entry.port = 0;
			CopyToPacketField(entry.ip, std::string());
		}

		if (!page.TryAddEntry(entry)) {
			flush();
			page.TryAddEntry(entry);
		}
	}
	flush();

	std::cout << "Server registry broadcast: " << totalServerCount << " servers in "
		<< pagesSent << " page(s)\n";
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
			dataPacket.objectsPerPlayer = gameInstance->GetObjectsPerPlayer();
			dataPacket.playerCount = gameInstance->GetPlayerCountToStartServers();

			mDistributedPhysicsManagerServer->SendGlobalReliablePacket(dataPacket);
			Profiler::SetConnectedGameClients(Profiler::GetConnectedGameClients() + 1);

			if (gameInstance->IsServersReadyToStart()) {
				StartGameServers(gameInstance->GetGameID());
			}
		}
	}
	}
}


void DistributedManager::SystemManager::HandleDistributedPhysicsClientConnectedPacketReceived(int peerNumber,
	NCL::CSC8503::DistributedPhysicsClientConnectedToManagerPacket* packet) {

	auto* serverData = DistributedUtils::CreatePhysicsServerData(packet->ipAddress, packet->physicsServerID, packet->gameInstanceID);
	// This object exists only because the server connected and registered, so it has
	// started by definition. The flag had a setter that nothing ever called, which made
	// CheckIsGameStartable's first condition permanently false.
	serverData->SetIsServerStarted(true);
	AddServerData(*serverData);

	int portForClientsToConnect = packet->physicsPacketDistributorPort;
	serverData->SetDataSenderPort(portForClientsToConnect);

	// The region this server owns, so the client can draw the partition. Sourced from the
	// instance's border map (empty string if the instance/server is somehow unknown - the
	// client tolerates that by simply not drawing the region).
	std::string borderStr;
	if (auto* gameInstance = mDistributedPhysicsManagerServer->GetGameInstance(packet->gameInstanceID)) {
		auto& borderMap = gameInstance->GetServerBorderStrMap();
		auto it = borderMap.find(packet->physicsServerID);
		if (it != borderMap.end()) {
			borderStr = it->second;
		}
	}

	std::cout << "Distributed Physics Server Info Packet Sent! IP: " << serverData->GetServerIPAddress()
		<< "| port: " << portForClientsToConnect << "| border: " << borderStr << std::endl;

	std::cout << "Sending physics server data packet to server: " << packet->physicsServerID << "\n";
	SendStartDataToPhysicsServer(packet->gameInstanceID, packet->physicsServerID);

	SendDistributedPhysicsServerInfoToClients(serverData->GetServerIPAddress(), packet->physicsServerID, portForClientsToConnect, borderStr);
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
	mPhysicsServerMiddlewareRunningInstanceMap.insert(newPair);

	Profiler::SetConnectedPhysicsServerMiddlewares(Profiler::GetConnectedPhysicsServerMiddlewares() + 1);

	SendPhysicsServerMiddlewareDataPacket(peerID, newMidwareID);
}

void DistributedManager::SystemManager::SendRunServerInstancePacket(int gameInstance, int physicsServerID, int midwareID, std::string borderStr) {
	RunDistributedPhysicsServerInstancePacket packet(physicsServerID, gameInstance, midwareID, borderStr.c_str());
	std::cout << "Server start border: " << packet.borderStr << "\n";
	mDistributedPhysicsManagerServer->SendGlobalReliablePacket(packet);
}

void DistributedManager::SystemManager::StartGameServers(int gameInstanceID) {
	auto* gameInstance = mDistributedPhysicsManagerServer->GetGameInstance(gameInstanceID);
	int maxServer = gameInstance->GetServerCount();

	for (int i = PHYSICS_SERVER_ID_BUFFER; i < PHYSICS_SERVER_ID_BUFFER + maxServer; i++) {

		std::cout << "Creating server(" << i << ") for game instance: " << gameInstance->GetGameID() << "\n";
		std::string serverBorderStr = gameInstance->GetServerBorderStrMap()[i].c_str();
		int midwareID = GetAvailablePhysicsMidware();
		std::cout << "Sending create server to midware with ID: " << midwareID << "\n";
		SendRunServerInstancePacket(gameInstance->GetGameID(), i, midwareID, serverBorderStr);
	}
	PHYSICS_SERVER_ID_BUFFER += maxServer;
}

// FOUR separate bugs used to cancel out here, and fixing any one of them alone stops
// the game starting at all:
//
//  - the readiness packet never assigned gameInstanceID, so this was called with
//    uninitialised stack (0xCCCCCCCC in a debug build);
//  - GetPhysicsServerDataList returned a reference to a function-local vector, so the
//    list read empty;
//  - SetIsServerStarted was never called anywhere, so that flag was always false;
//  - SetIsAllClientsConnectedToServer had an EMPTY BODY, so that flag was too.
//
// The first two made the list empty, so the loop body never ran and this returned true
// immediately - which is the only reason the system ever booted. The last two were
// completely masked by that and, as far as the code shows, had never worked.
//
// A DistributedPhysicsServerData exists only because a server connected and
// registered, so "started" is now set at registration, the setter has a body, and this
// checks what it always claimed to: every server registered for the instance has
// reported that its clients are connected.
bool DistributedManager::SystemManager::CheckIsGameStartable(int gameInstanceID) {
	const std::vector<DistributedPhysicsServerData*> serverList =
		GetPhysicsServerDataList(gameInstanceID);
	if (serverList.empty()) {
		return false;   // Nothing registered for this instance yet.
	}
	for (const auto& server : serverList) {
		if (!server->GetIsServerStarted() || !server->GetIsAllClientsConnectedToServer()) {
			return false;
		}
	}

	return true;
}

// By value. This returned a reference to a function-local vector, so every caller
// iterated a destroyed container - undefined behaviour that happened to read as empty.
std::vector<DistributedPhysicsServerData*> DistributedManager::SystemManager::GetPhysicsServerDataList(
	int gameInstanceID) const {

	std::vector<DistributedPhysicsServerData*> dataList;

	for (const auto& serverData : mDistributedPhysicsServers) {
		if (serverData->GetGameInstanceID() == gameInstanceID) {
			dataList.push_back(serverData);
		}
	}

	return dataList;
}

int DistributedManager::SystemManager::GetAvailablePhysicsMidware() {
	const auto& firstMidware = mPhysicsServerMiddlewareRunningInstanceMap.begin();
	int minInstanceCount = firstMidware->second;
	int midwareID = firstMidware->first;
	for (const auto& midware : mPhysicsServerMiddlewareRunningInstanceMap) {
		if (midware.second < minInstanceCount) {
			minInstanceCount = midware.second;
			midwareID = midware.first;
		}
	}
	mPhysicsServerMiddlewareRunningInstanceMap[midwareID] = ++minInstanceCount;
	return midwareID;
}


NCL::GameInstance* DistributedManager::SystemManager::CreateNewGameInstance(int maxServer, int clientCount, int objectsPerPlayer,
	double worldMinX, double worldMaxX, double worldMinZ, double worldMaxZ) {
	GameInstance* newGame = new GameInstance(++GAME_INSTANCE_ID_BUFFER, maxServer, PHYSICS_SERVER_ID_BUFFER, clientCount, objectsPerPlayer,
		worldMinX, worldMaxX, worldMinZ, worldMaxZ);
	mDistributedPhysicsManagerServer->AddGameInstance(newGame);


	return newGame;
}

int DistributedManager::SystemManager::GetConnectedMidwareCount() const {
	return static_cast<int>(mPhysicsServerMiddlewareRunningInstanceMap.size());
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
