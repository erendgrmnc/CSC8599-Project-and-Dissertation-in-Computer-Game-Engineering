#include "DistributedMultiplayerGameScene.h"

#include "GameClient.h"
#include "GameWorld.h"
#include "RenderObject.h"

using namespace NCL::CSC8503;
using namespace NCL::Maths;

DistributedMultiplayerGameScene::DistributedMultiplayerGameScene() {
	mClientSideLastFullID = 0;
	mServerSideLastFullID = 0;

	NetworkBase::Initialise();
}

DistributedMultiplayerGameScene::~DistributedMultiplayerGameScene() {
	mDistributedManagerClient->Disconnect();

	for (const auto& client : mDistributedPhysicsClients) {
		client->Disconnect();
	}
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
	const std::string& playerName) {
	auto* client = new NCL::CSC8503::GameClient();
	const bool isConnected = client->Connect(a, b, c, d, port, playerName);

	if (isConnected) {
		client->RegisterPacketHandler(Delta_State, this);
		client->RegisterPacketHandler(Full_State, this);
		client->RegisterPacketHandler(Player_Connected, this);
		client->RegisterPacketHandler(Player_Disconnected, this);
		client->RegisterPacketHandler(String_Message, this);
	}

	mDistributedPhysicsClients.push_back(client);

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
	if (!mWorld || !mObjMesh || !mObjShader) {
		return nullptr; // headless / no render resources - receive only.
	}

	std::cout << "Spawning replica for network object " << objectID << "..." << std::endl;
	auto* obj = new GameObject(NoSpecialFeatures, "NetObject " + std::to_string(objectID));
	const float scale = 4.0f;
	obj->GetTransform().SetScale(Vector3(scale, scale, scale));

	const float cullRadius = scale * 1.75f;
	obj->SetRenderObject(new RenderObject(&obj->GetTransform(), mObjMesh, mObjAlbedo, mObjNormal, mObjShader, cullRadius));
	obj->GetRenderObject()->SetColour(Vector4(0.30f, 0.70f, 1.00f, 1.0f));

	auto* netObj = new NetworkObject(*obj, objectID);
	obj->SetNetworkObject(netObj);
	mNetworkObjects.push_back(netObj);
	mWorld->AddGameObject(obj);

	std::cout << "Spawned client replica for network object " << objectID << std::endl;
	return netObj;
}

void DistributedMultiplayerGameScene::HandleFullPacket(FullPacket* packet) {
	NetworkObject* netObj = FindNetworkObject(packet->objectID);
	if (!netObj) {
		netObj = SpawnReplica(packet->objectID);
	}
	if (netObj) {
		netObj->ReadPacket(*packet);
	}
}

void DistributedMultiplayerGameScene::HandleDeltaPacket(DeltaPacket* packet) {
	NetworkObject* netObj = FindNetworkObject(packet->objectID);
	if (netObj) {
		netObj->ReadPacket(*packet);
	}
}

void DistributedMultiplayerGameScene::UpdatePhysicsClients(float dt) const {
	for (const auto& client : mDistributedPhysicsClients) {
		client->UpdateClient();
	}
}

void DistributedMultiplayerGameScene::HandleOnConnectToDistributedPhysicsServerPacketReceived(
	DistributedClientConnectToPhysicsServerPacket* packet) {

	std::cout << "Routing to physics server: '" << packet->ipAddress << "' port "
		<< packet->physicsPacketDistributorPort << std::endl;

	std::vector<char> ipOctets;
	try {
		ipOctets = IpToCharArray(packet->ipAddress);
	}
	catch (const std::exception& e) {
		std::cout << "  invalid physics-server IP, skipping connect: " << e.what() << std::endl;
		return;
	}

	ConnectClientToDistributedGameServer(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], packet->physicsPacketDistributorPort, "");
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
