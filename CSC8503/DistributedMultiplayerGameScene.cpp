#include "DistributedMultiplayerGameScene.h"

#include "GameClient.h"
#include "GameWorld.h"
#include "GameObject.h"
#include "Profiler.h"
#include "RenderObject.h"
#include "DistributedSystemCommonFiles/DistributedUtils.h"

#include <algorithm>
#include <limits>

using namespace NCL;
using namespace NCL::CSC8503;
using namespace NCL::Maths;

DistributedMultiplayerGameScene::DistributedMultiplayerGameScene() {
	mClientSideLastFullID = 0;
	mServerSideLastFullID = 0;

	// The client needs the same registry the servers have: SendCommand looks up the
	// command to derive its scope and run client-side validation before routing.
	NCL::Interaction::CommandRegistry::RegisterDefaults();

	NetworkBase::Initialise();
}

DistributedMultiplayerGameScene::~DistributedMultiplayerGameScene() {
	mDistributedManagerClient->Disconnect();

	for (const auto& link : mDistributedPhysicsClients) {
		link.client->Disconnect();
	}

	// Replicas created without a GameWorld are owned here (see SpawnReplica); the
	// rendered path's objects belong to the world and are freed with it.
	for (auto* obj : mHeadlessReplicas) {
		delete obj;
	}
	mHeadlessReplicas.clear();
}

bool DistributedMultiplayerGameScene::ConnectClientToDistributedManager(char a, char b, char c, char d, int port) {
	mDistributedManagerClient = new NCL::CSC8503::GameClient();

	// Announce ourselves to the manager once the connection is established, so it
	// adds us to the game instance and - once the expected client count is reached -
	// starts the physics servers. This join step previously lived in the removed
	// team-game scene; without it the manager never spawns the game servers.
	mDistributedManagerClient->AddOnClientConnected([this] {
		SendGameClientConnectedPacket(mGameInstanceId);
	});

	const bool isConnected = mDistributedManagerClient->Connect(a, b, c, d, port, "");

	if (isConnected) {
		mDistributedManagerClient->RegisterPacketHandler(DistributedClientConnectToPhysicsServer, this);
		mDistributedManagerClient->RegisterPacketHandler(GameStartState, this);
	}

	return isConnected;
}

void DistributedMultiplayerGameScene::SendGameClientConnectedPacket(int gameInstanceID) {
	std::cout << "Sending game client connect packet for game instance: " << gameInstanceID << "\n";
	DistributedClientConnectedToSystemPacket packet(gameInstanceID, DistributedSystemClientType::DistributedGameClient);
	mDistributedManagerClient->SendPacket(packet);
}

bool DistributedMultiplayerGameScene::ConnectClientToDistributedGameServer(char a, char b, char c, char d, int port,
	const std::string& playerName, int serverId) {
	auto* client = new NCL::CSC8503::GameClient();
	const bool isConnected = client->Connect(a, b, c, d, port, playerName);

	if (isConnected) {
		client->RegisterPacketHandler(Delta_State, this);
		client->RegisterPacketHandler(Full_State, this);
		client->RegisterPacketHandler(Player_Connected, this);
		client->RegisterPacketHandler(Player_Disconnected, this);
		client->RegisterPacketHandler(String_Message, this);
		client->RegisterPacketHandler(DistributedCommandAck, this);
		client->RegisterPacketHandler(DistributedObjectDespawned, this);
	}

	mDistributedPhysicsClients.push_back({ client, serverId });

	return isConnected;
}

void DistributedMultiplayerGameScene::UpdateGame(float dt) {
	UpdateDistributedManagerClient(dt);
	UpdatePhysicsClients(dt);
}

void DistributedMultiplayerGameScene::UpdateDistributedManagerClient(float dt) {
	mDistributedManagerClient->UpdateClient();
}

void DistributedMultiplayerGameScene::ReceivePacket(int type, GamePacket* payload, int source) {
	switch (type) {
	case BasicNetworkMessages::DistributedClientConnectToPhysicsServer: {
		DistributedClientConnectToPhysicsServerPacket* packet = static_cast<DistributedClientConnectToPhysicsServerPacket*>(payload);
		HandleOnConnectToDistributedPhysicsServerPacketReceived(packet);
		break;
	}
	case BasicNetworkMessages::GameStartState: {
		HandleGameStartPacketReceived(static_cast<GameStartStatePacket*>(payload));
		break;
	}
	case BasicNetworkMessages::Full_State: {
		HandleFullPacket(static_cast<FullPacket*>(payload));
		break;
	}
	case BasicNetworkMessages::Delta_State: {
		HandleDeltaPacket(static_cast<DeltaPacket*>(payload));
		break;
	}
	case BasicNetworkMessages::DistributedObjectDespawned: {
		auto* despawn = static_cast<DistributedObjectDespawnedPacket*>(payload);
		mTombstones.insert(despawn->objectID);

		// Tear the replica down locally. Ids are never recycled, so the tombstone is
		// permanently safe and a later snapshot for this id can be rejected outright.
		for (auto it = mNetworkObjects.begin(); it != mNetworkObjects.end(); ++it) {
			if ((*it)->GetNetworkID() == despawn->objectID) {
				mNetworkObjects.erase(it);
				break;
			}
		}
		mObjectOwner.erase(despawn->objectID);
		break;
	}
	case BasicNetworkMessages::DistributedCommandAck: {
		auto* ack = static_cast<DistributedCommandAckPacket*>(payload);
		++mAckResultCounts[ack->result];

		// A NotOwner ack carries the true owner. Adopting it immediately is what
		// stops a stale owner table from misrouting every subsequent command for
		// this object.
		if (ack->result == static_cast<int>(NCL::Interaction::CommandResult::NotOwner) &&
			ack->correctedServerID >= 0 && ack->targetObjectID >= 0) {
			mObjectOwner[ack->targetObjectID] = ack->correctedServerID;
		}
		break;
	}
	default:
		std::cout << "Received unknown packet. Type: " << payload->type << std::endl;
		break;
	}
}

int DistributedMultiplayerGameScene::ResolveCommandTarget(
	const NCL::Interaction::CommandArgs& args,
	const NCL::Interaction::CommandScope& scope) const {

	if (scope.targetsObject && args.targetObjectID >= 0) {
		const auto owner = mObjectOwner.find(args.targetObjectID);
		if (owner != mObjectOwner.end()) {
			return owner->second;
		}
		// Fall through: the object may still be resolvable by position even if the
		// owner table has not seen it yet.
	}

	if (scope.targetsPoint) {
		// The SAME function the servers use, not a re-implementation of the same
		// rule: a client that disagreed about a border would misroute every command
		// issued on it, and the disagreement would be invisible until it happened.
		std::vector<NCL::Interaction::RegionBounds> regions;
		regions.reserve(mServerRegions.size());
		for (const ServerRegion& region : mServerRegions) {
			regions.push_back(NCL::Interaction::RegionBounds{
				region.serverId, region.minX, region.maxX, region.minZ, region.maxZ });
		}
		return NCL::Interaction::OwningServerFor(regions, args.worldPoint);
	}

	// Deliberately no broadcast fallback: N servers would each apply the command.
	return -1;
}

int DistributedMultiplayerGameScene::PickDestroyCandidate() {
	if (mNetworkObjects.empty()) {
		return -1;
	}
	// Rotate rather than always taking the first: otherwise the driver would keep
	// re-targeting an object it has already destroyed and measure nothing.
	for (size_t attempt = 0; attempt < mNetworkObjects.size(); ++attempt) {
		mDestroyCursor = (mDestroyCursor + 1) % mNetworkObjects.size();
		const int id = mNetworkObjects[mDestroyCursor]->GetNetworkID();
		if (mTombstones.find(id) == mTombstones.end()) {
			return id;
		}
	}
	return -1;
}

std::vector<int> DistributedMultiplayerGameScene::GetConnectedServerIds() const {
	std::vector<int> ids;
	ids.reserve(mDistributedPhysicsClients.size());
	for (const PhysicsServerLink& link : mDistributedPhysicsClients) {
		ids.push_back(link.serverId);
	}
	return ids;
}

bool DistributedMultiplayerGameScene::SendCommand(NCL::Interaction::CommandType type,
	NCL::Interaction::CommandArgs args) {
	return SendCommandTo(type, args, -1);
}

bool DistributedMultiplayerGameScene::SendCommandTo(NCL::Interaction::CommandType type,
	NCL::Interaction::CommandArgs args, int forcedServerId) {

	NCL::Interaction::IInteractionCommand* command =
		NCL::Interaction::CommandRegistry::Instance().Find(type);
	if (command == nullptr) {
		return false;
	}

	// Client-side validation is an optimisation only - the server validates again.
	if (!command->Validate(args)) {
		return false;
	}

	const NCL::Interaction::CommandScope scope = command->GetScope(args);

	// Stamp the object's last known position onto an object-targeted command, so a
	// server that receives it but holds nothing for the object can still work out who
	// owns it. Without this every server would have to be told at startup where every
	// object it does NOT own lives - O(world) per server, which is exactly the cost
	// region-local state exists to remove. Only for object-targeted commands: for a
	// point-targeted one worldPoint is the command's own argument and must not be
	// overwritten.
	if (scope.targetsObject && args.targetObjectID >= 0) {
		if (NetworkObject* replica = FindNetworkObject(args.targetObjectID)) {
			args.worldPoint = replica->GetGameObject().GetTransform().GetPosition();
			args.flags |= static_cast<int>(NCL::Interaction::CommandFlags::HasObjectPosition);
		}
	}

	const int targetServerId = (forcedServerId >= 0)
		? forcedServerId
		: ResolveCommandTarget(args, scope);
	if (targetServerId < 0) {
		std::cout << "Command dropped: no server resolved for object "
			<< args.targetObjectID << ".\n";
		return false;
	}

	GameClient* link = nullptr;
	for (const PhysicsServerLink& serverLink : mDistributedPhysicsClients) {
		if (serverLink.serverId == targetServerId) {
			link = serverLink.client;
			break;
		}
	}
	if (link == nullptr) {
		return false;
	}

	// Continuous input is state and is re-sent every tick, so it is not sequenced;
	// giving it a sequence would consume the dedupe window in a few seconds.
	const int sequence = scope.isContinuous ? 0 : mNextCommandSequence++;

	DistributedClientCommandPacket packet(static_cast<int>(type), sequence, targetServerId, args);
	link->SendReliablePacket(packet);
	++mCommandsSent;
	return true;
}

void DistributedMultiplayerGameScene::SetRenderResources(NCL::CSC8503::GameWorld* world, NCL::Rendering::Mesh* mesh,
	NCL::Rendering::Texture* albedo, NCL::Rendering::Texture* normal, NCL::Rendering::Shader* shader) {
	mWorld = world;
	mObjMesh = mesh;
	mObjAlbedo = albedo;
	mObjNormal = normal;
	mObjShader = shader;
}

NetworkObject* DistributedMultiplayerGameScene::FindNetworkObject(int objectID) {
	for (auto* netObj : mNetworkObjects) {
		if (netObj->GetNetworkID() == objectID) {
			return netObj;
		}
	}
	return nullptr;
}

// Creates one visible cube replica for a network object the client hasn't seen
// before. The transform is set by the incoming snapshot (ReadPacket); we only
// pick a visible scale + colour here.
NetworkObject* DistributedMultiplayerGameScene::SpawnReplica(int objectID) {
	// Invariant I3, no resurrection: a snapshot for a destroyed object must never
	// recreate it. Snapshots already in flight when the despawn was sent will arrive
	// afterwards, so this is the normal case, not an error - it is counted rather
	// than logged so the rate stays visible.
	if (mTombstones.find(objectID) != mTombstones.end()) {
		++mResurrectionAttempts;
		return nullptr;
	}

	// Replica STATE is created unconditionally; only the visual representation
	// depends on render resources.
	//
	// This used to bail out entirely when there was no renderer, which silently
	// disabled the client's whole network path in headless mode: no NetworkObject
	// meant no full snapshot was ever applied, so no snapshot acknowledgement was
	// ever sent, so the server had no delta baseline and every delta the client
	// received was discarded. A headless client consumed bandwidth and measured
	// nothing - which would have quietly invalidated any unattended experiment run.
	auto* obj = new GameObject(NoSpecialFeatures, "NetObject " + std::to_string(objectID));
	const float scale = 4.0f;
	obj->GetTransform().SetScale(Vector3(scale, scale, scale));

	const bool canRender = (mWorld != nullptr && mObjMesh != nullptr && mObjShader != nullptr);
	if (canRender) {
		const float cullRadius = scale * 1.75f;
		obj->SetRenderObject(new RenderObject(&obj->GetTransform(), mObjMesh, mObjAlbedo, mObjNormal, mObjShader, cullRadius));
		obj->GetRenderObject()->SetColour(Vector4(0.30f, 0.70f, 1.00f, 1.0f));
	}

	auto* netObj = new NetworkObject(*obj, objectID);
	obj->SetNetworkObject(netObj);
	mNetworkObjects.push_back(netObj);

	// Only the rendered path needs the object in the GameWorld; headless keeps it
	// alive through mNetworkObjects alone.
	if (mWorld) {
		mWorld->AddGameObject(obj);
	}
	else {
		mHeadlessReplicas.push_back(obj);
	}

	return netObj;
}

void DistributedMultiplayerGameScene::HandleFullPacket(FullPacket* packet) {
	NetworkObject* netObj = FindNetworkObject(packet->objectID);
	if (!netObj) {
		netObj = SpawnReplica(packet->objectID);
	}
	if (netObj) {
		if (netObj->ReadPacket(*packet)) {
			Profiler::RecordFullApplied();
		}
		ApplyOwnerColour(netObj, mActiveServerId);

		// A full snapshot arrives as one packet per object, so record the newest
		// state we have applied here and acknowledge it once per pump in
		// UpdatePhysicsClients - acking per packet would send one ack per object.
		if (mActiveServerId >= 0) {
			int& newest = mLastFullStateIdPerServer[mActiveServerId];
			newest = std::max(newest, packet->fullState.stateID);
		}
	}
}

void DistributedMultiplayerGameScene::HandleDeltaPacket(DeltaPacket* packet) {
	NetworkObject* netObj = FindNetworkObject(packet->objectID);
	if (netObj) {
		// ReadPacket's return value was being discarded, which is why deltas silently
		// failing to apply went unnoticed for so long. Count both outcomes.
		if (netObj->ReadPacket(*packet)) {
			Profiler::RecordDeltaApplied();
		}
		else {
			Profiler::RecordDeltaRejected();
		}
		ApplyOwnerColour(netObj, mActiveServerId);
	}
	else {
		Profiler::RecordDeltaRejected();
	}
}

// Pumps each physics client, marking which server "owns" the current pump so the
// packet handlers (which run synchronously inside UpdateClient) can attribute the
// snapshot to it. mActiveServerId is -1 outside the loop.
void DistributedMultiplayerGameScene::UpdatePhysicsClients(float dt) {
	for (const auto& link : mDistributedPhysicsClients) {
		mActiveServerId = link.serverId;
		link.client->UpdateClient();
	}
	mActiveServerId = -1;

	SendSnapshotAcks();
}

// Tells each physics server which full snapshot we have applied. The server encodes
// its deltas against the oldest state still unacknowledged across all clients, so
// without these acks it has no baseline and every delta it sends is discarded.
void DistributedMultiplayerGameScene::SendSnapshotAcks() {
	for (const auto& link : mDistributedPhysicsClients) {
		auto newest = mLastFullStateIdPerServer.find(link.serverId);
		if (newest == mLastFullStateIdPerServer.end()) {
			continue;
		}

		int& lastAcked = mLastAckedStateIdPerServer[link.serverId];
		if (newest->second <= lastAcked) {
			continue;
		}

		DistributedClientSnapshotAckPacket packet(newest->second, link.serverId);
		link.client->SendPacket(packet);
		lastAcked = newest->second;
	}
}

void DistributedMultiplayerGameScene::HandleOnConnectToDistributedPhysicsServerPacketReceived(
	DistributedClientConnectToPhysicsServerPacket* packet) {

	std::cout << "Routing to physics server " << packet->physicsServerID << ": '" << packet->ipAddress << "' port "
		<< packet->physicsPacketDistributorPort << " border '" << packet->borderStr << "'" << std::endl;

	std::vector<char> ipOctets;
	try {
		ipOctets = IpToCharArray(packet->ipAddress);
	}
	catch (const std::exception& e) {
		std::cout << "  invalid physics-server IP, skipping connect: " << e.what() << std::endl;
		return;
	}

	// Record the server's region for the overlay. A malformed border is not fatal - the
	// client still connects and receives snapshots, that region just isn't drawn.
	ServerRegion region;
	region.serverId = packet->physicsServerID;
	if (DistributedUtils::ParseBorderString(packet->borderStr, region.minX, region.maxX, region.minZ, region.maxZ)) {
		region.colour = ColourForServer(region.serverId);
		mServerRegions.push_back(region);
	}
	else {
		std::cout << "  malformed border string, region not drawn." << std::endl;
	}

	ConnectClientToDistributedGameServer(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], packet->physicsPacketDistributorPort, "", packet->physicsServerID);
	std::cout << "  connected to physics server." << std::endl;
}

void DistributedMultiplayerGameScene::HandleGameStartPacketReceived(GameStartStatePacket* packet) {
	mIsGameStarted = packet->isGameStarted;
}

std::vector<char> DistributedMultiplayerGameScene::IpToCharArray(const std::string& ipAddress) {
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

// Fixed per-server palette, indexed serverId % 8. serverId < 0 (unknown owner) gets the
// neutral blue that ungrouped replicas use.
Vector4 DistributedMultiplayerGameScene::ColourForServer(int serverId) {
	static const Vector4 palette[8] = {
		Vector4(0.20f, 0.80f, 1.00f, 1.0f), // cyan
		Vector4(1.00f, 0.55f, 0.15f, 1.0f), // orange
		Vector4(0.40f, 0.90f, 0.35f, 1.0f), // green
		Vector4(1.00f, 0.35f, 0.85f, 1.0f), // magenta
		Vector4(1.00f, 0.90f, 0.25f, 1.0f), // yellow
		Vector4(1.00f, 0.30f, 0.30f, 1.0f), // red
		Vector4(0.65f, 0.45f, 1.00f, 1.0f), // violet
		Vector4(0.20f, 0.85f, 0.75f, 1.0f), // teal
	};
	if (serverId < 0) {
		return Vector4(0.30f, 0.70f, 1.00f, 1.0f); // default replica blue
	}
	return palette[serverId % 8];
}

void DistributedMultiplayerGameScene::ApplyOwnerColour(NetworkObject* netObj, int serverId) {
	if (!netObj) {
		return;
	}
	if (serverId >= 0) {
		mObjectOwner[netObj->GetNetworkID()] = serverId;
	}
	RenderObject* ro = netObj->GetGameObject().GetRenderObject();
	if (!ro) {
		return;
	}
	ro->SetColour(ColourForServer(mOverlayEnabled ? serverId : -1));
}

void DistributedMultiplayerGameScene::SetOverlayEnabled(bool enabled) {
	mOverlayEnabled = enabled;
	std::cout << "Region overlay " << (enabled ? "ON" : "OFF") << std::endl;

	// Recolour existing replicas from their remembered owner so the toggle takes effect
	// immediately, not on the next snapshot.
	for (NetworkObject* netObj : mNetworkObjects) {
		RenderObject* ro = netObj->GetGameObject().GetRenderObject();
		if (!ro) {
			continue;
		}
		int owner = -1;
		auto it = mObjectOwner.find(netObj->GetNetworkID());
		if (it != mObjectOwner.end()) {
			owner = it->second;
		}
		ro->SetColour(ColourForServer(enabled ? owner : -1));
	}
}

bool DistributedMultiplayerGameScene::GetWorldBounds(float& minX, float& maxX, float& minZ, float& maxZ) const {
	if (mServerRegions.empty()) {
		return false;
	}
	minX = minZ = std::numeric_limits<float>::max();
	maxX = maxZ = std::numeric_limits<float>::lowest();
	for (const ServerRegion& r : mServerRegions) {
		minX = std::min(minX, r.minX);
		maxX = std::max(maxX, r.maxX);
		minZ = std::min(minZ, r.minZ);
		maxZ = std::max(maxZ, r.maxZ);
	}
	return true;
}
