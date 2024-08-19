#include "ServerMidwareManager.h"

#include "AnimationSystem.h"
#include "GameClient.h"
#include "NetworkObject.h"
#include "DistributedSystemCommonFiles/DistributedUtils.h"

namespace {
	const std::string PHYSIC_SERVER_PATH = "./DistributedPhysicsServer/EntryPoint.exe";
}

NCL::ServerMidwareManager::ServerMidwareManager() : mDistributedManagerClient(nullptr) {
	distributedManagerPort = -1;
	int mMidwareID = -1;
	NetworkBase::Initialise();
}

NCL::ServerMidwareManager::~ServerMidwareManager() {
	delete mDistributedManagerClient;
}

void NCL::ServerMidwareManager::ConnectToDistributedManager(std::string& ipAddress, int port) {
	distributedManagerPort = port;
	serverManagerIpAddress = ipAddress;

	const std::vector<char> ipOctests = NCL::DistributedUtils::ConvertIpStrToCharArr(ipAddress);
	auto* client = new NCL::CSC8503::GameClient();

	std::function<void()> callback = [this, client] {
		const std::string& ipAddress = client->GetIPAddress();
		SendMidwareConnectedPacket(ipAddress);
	};

	client->AddOnClientConnected(callback);

	bool isConnected = client->Connect(ipOctests[0], ipOctests[1], ipOctests[2], ipOctests[3], port, "");
	mDistributedManagerClient = client;
	if (isConnected) {
		RegisterDistributedManagerClientPackets();
	}
}

void NCL::ServerMidwareManager::ReceivePacket(int type, GamePacket* payload, int source) {
	switch (type) {
	case BasicNetworkMessages::PhysicsServerMiddlewareData: {
		auto* packet = static_cast<PhysicsServerMiddlewareDataPacket*>(payload);
		std::cout << "Received peer ID: " << packet->peerID << "| Midware peer ID: " << mDistributedManagerClient->GetPeerID() << "\n";
		if (mDistributedManagerClient->GetPeerID() -1 == packet->peerID) {
			HandleMidwareDataPacket(packet);
		}
		break;
	}
	case BasicNetworkMessages::RunDistributedPhysicsServerInstance: {
		RunDistributedPhysicsServerInstancePacket* packet = static_cast<RunDistributedPhysicsServerInstancePacket*>(payload);
		std::cout << "Midware ID from packet: " << packet->midwareID << " | Midware ID: " << mMidwareID<< "\n";
		if (packet->midwareID == mMidwareID) {
			HandleRunInstancePacket(packet);
		}
		break;
	}
	default:
		std::cout << "Received unknown packet. Type: " << payload->type << std::endl;
		break;
	}
}

void NCL::ServerMidwareManager::Update(float dt) {
	if (mDistributedManagerClient) {
		mDistributedManagerClient->UpdateClient();
	}
}

void NCL::ServerMidwareManager::RegisterDistributedManagerClientPackets() {
	mDistributedManagerClient->RegisterPacketHandler(BasicNetworkMessages::RunDistributedPhysicsServerInstance, this);
	mDistributedManagerClient->RegisterPacketHandler(BasicNetworkMessages::PhysicsServerMiddlewareData, this);
}

void NCL::ServerMidwareManager::HandleRunInstancePacket(RunDistributedPhysicsServerInstancePacket* packet) {
	StartPhysicsServerInstance(distributedManagerPort, packet->serverID, packet->gameInstanceID, packet->borderStr);
}

void ServerMidwareManager::StartPhysicsServerInstance(int distributedManagerPort, int physicsServerID, int gameInstanceID,
	std::string& borderStr) {

	int physicsServerId = physicsServerID;

	const std::string& serverBordersStr = borderStr;
	std::string arguments = "--arg1 " + serverManagerIpAddress + "-" + std::to_string(distributedManagerPort) + "-" + std::to_string(physicsServerId) +"-" + std::to_string(gameInstanceID) +"-" + serverBordersStr;

	std::cout << arguments << std::endl;
	std::thread programThread([this, physicsServerId, arguments]() {
		ServerMidwareManager::ExecutePhysicsServerProgram(PHYSIC_SERVER_PATH, arguments, physicsServerId);
		});

	programThread.detach();
}

void ServerMidwareManager::ExecutePhysicsServerProgram(const std::string& programPath, const std::string& arguments,
	int serverId) {

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	std::string commandLine = programPath + " " + arguments;

	char* commandLineCStr = const_cast<char*>(commandLine.c_str());

	std::cout << "Command Line arguments: " << commandLineCStr << '\n';

	if (!CreateProcessA(
		programPath.c_str(),   // Program path
		commandLineCStr,       // Command line
		NULL,                  // Process handle not inheritable
		NULL,                  // Thread handle not inheritable
		FALSE,                 // Set handle inheritance to FALSE
		CREATE_NEW_CONSOLE,    // Creation flags: CREATE_NEW_CONSOLE to create a new console window
		NULL,                  // Use parent's environment block
		NULL,                  // Use parent's starting directory 
		&si,                   // Pointer to STARTUPINFO structure
		&pi)                   // Pointer to PROCESS_INFORMATION structure
		) {
		std::cerr << "CreateProcess failed (" << GetLastError() << ")" << std::endl;
	}
}

void ServerMidwareManager::HandleMidwareDataPacket(CSC8503::PhysicsServerMiddlewareDataPacket* packet) {
	mMidwareID = packet->middlewareID;
	std::cout << "Received middleware ID: " << mMidwareID << "\n";
}

void ServerMidwareManager::SendMidwareConnectedPacket(const std::string& ipAddress) {
	PhysicsServerMiddlewareConnectedPacket packet(ipAddress);
	mDistributedManagerClient->SendReliablePacket(packet);
}
