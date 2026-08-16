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
	default:
		std::cout << "Received unknown packet. Type: " << payload->type << std::endl;
		break;
	}
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
