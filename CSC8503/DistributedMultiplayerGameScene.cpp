#include "DistributedMultiplayerGameScene.h"

#include "GameClient.h"

DistributedMultiplayerGameScene::DistributedMultiplayerGameScene() {
	mThisServer = nullptr;
	mThisClient = nullptr;

	mClientSideLastFullID = 0;
	mServerSideLastFullID = 0;

	mGameState = NCL::CSC8503::MainMenuState;

	NetworkBase::Initialise();
	mTimeToNextPacket = 0.0f;
	mPacketsToSnapshot = 0;
}

DistributedMultiplayerGameScene::~DistributedMultiplayerGameScene() {
	mDistributedManagerClient->Disconnect();

	for (const auto& client : mDistributedPhysicsClients) {
		client->Disconnect();
	}
}

bool DistributedMultiplayerGameScene::ConnectClientToDistributedManager(char a, char b, char c, char d, int port) {
	mDistributedManagerClient = new NCL::CSC8503::GameClient();
	const bool isConnected = mDistributedManagerClient->Connect(a, b, c, d, port, "");

	if (isConnected) {
		mDistributedManagerClient->RegisterPacketHandler(DistributedClientConnectToPhysicsServer, this);
		mDistributedManagerClient->RegisterPacketHandler(GameStartState, this);
	}

	return isConnected;
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

		break;
	}
	default:
		std::cout << "Received unknown packet. Type: " << payload->type << std::endl;
		break;
	}
}

void DistributedMultiplayerGameScene::UpdatePhysicsClients(float dt) const {
	for (const auto& client : mDistributedPhysicsClients) {
		client->UpdateClient();
	}
}

void DistributedMultiplayerGameScene::SetItemsLeftToZero() {

}

void DistributedMultiplayerGameScene::HandleOnConnectToDistributedPhysicsServerPacketReceived(
	DistributedClientConnectToPhysicsServerPacket* packet) {

	std::vector<char> ipOctets = IpToCharArray(packet->ipAddress);
	std::cout << "Connecting to connecting physics server on: " << packet->ipAddress << " | " << packet->physicsPacketDistributerPort << "\n";

	ConnectClientToDistributedGameServer(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], packet->physicsPacketDistributerPort, "");
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
