#include "ServerMidwareManager.h"

#include <Windows.h>
#include <mutex>
#include <vector>

#include "GameClient.h"
#include "NetworkObject.h"
#include "DistributedSystemCommonFiles/DistributedUtils.h"

using namespace NCL;
using namespace NCL::CSC8503;

namespace {
	// Serializes stdout writes from the per-server reader threads so forwarded
	// lines don't interleave mid-line.
	std::mutex gStdOutMutex;
}

namespace {
	const std::string PHYSICS_SERVER_PATH = "./DistributedPhysicsServer/EntryPoint.exe";
}

NCL::ServerMidwareManager::ServerMidwareManager() : mDistributedManagerClient(nullptr) {
	distributedManagerPort = -1;
	int mMidwareID = -1;
	mServerExePath = PHYSICS_SERVER_PATH;
	NetworkBase::Initialise();
}

void NCL::ServerMidwareManager::SetServerExePath(const std::string& path) {
	if (!path.empty()) {
		mServerExePath = path;
	}
}

void NCL::ServerMidwareManager::SetServerExtraArgs(const std::string& args) {
	mServerExtraArgs = args;
}

void NCL::ServerMidwareManager::SetHeadless(bool headless) {
	mHeadless = headless;
}

NCL::ServerMidwareManager::~ServerMidwareManager() {
	delete mDistributedManagerClient;
}

void NCL::ServerMidwareManager::ConnectToDistributedManager(std::string& ipAddress, int port) {
	distributedManagerPort = port;
	serverManagerIpAddress = ipAddress;

	const std::vector<char> ipOctets = NCL::DistributedUtils::ConvertIpStrToCharArr(ipAddress);
	auto* client = new NCL::CSC8503::GameClient();

	std::function<void()> callback = [this, client] {
		const std::string& ipAddress = client->GetIPAddress();
		SendMidwareConnectedPacket(ipAddress);
	};

	client->AddOnClientConnected(callback);

	bool isConnected = client->Connect(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], port, "");
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
	std::string serverBorderStr(packet->borderStr);
	StartPhysicsServerInstance(distributedManagerPort, packet->serverID, packet->gameInstanceID, serverBorderStr);
}

void ServerMidwareManager::StartPhysicsServerInstance(int distributedManagerPort, int physicsServerID, int gameInstanceID,
	std::string& borderStr) {

	int physicsServerId = physicsServerID;

	const std::string& serverBordersStr = borderStr;
	std::string arguments = "--arg1 " + serverManagerIpAddress + "-" + std::to_string(distributedManagerPort) + "-" + std::to_string(physicsServerId) +"-" + std::to_string(gameInstanceID) +"-" + serverBordersStr;

	// Run spawned servers in the same mode as this midware. Headless servers are
	// windowless and their stdout is forwarded to the launcher via this midware.
	if (mHeadless) {
		arguments += " --headless";
	}

	// Determinism and any other pass-through flags. Without this the game servers
	// silently run with an adaptive timestep and an unseeded world even when the
	// operator asked for a reproducible run, because nothing rejects the flags at
	// the level they were set.
	if (!mServerExtraArgs.empty()) {
		arguments += " " + mServerExtraArgs;
	}

	std::cout << arguments << std::endl;
	const std::string serverExePath = mServerExePath;
	std::thread programThread([this, physicsServerId, arguments, serverExePath]() {
		ServerMidwareManager::ExecutePhysicsServerProgram(serverExePath, arguments, physicsServerId);
		});

	programThread.detach();
}

void ServerMidwareManager::ExecutePhysicsServerProgram(const std::string& programPath, const std::string& arguments,
	int serverId) {

	const std::string tag = "[server " + std::to_string(serverId) + "] ";

	// Pipe the child's stdout/stderr back so the launcher (which captures THIS
	// midware's stdout, locally or forwarded via the agent) sees the game-server
	// logs and @@STAT telemetry without a separate console window per server.
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = nullptr;

	HANDLE childStdOutRead = nullptr;
	HANDLE childStdOutWrite = nullptr;
	if (!CreatePipe(&childStdOutRead, &childStdOutWrite, &sa, 0)) {
		std::cerr << tag << "CreatePipe failed (" << GetLastError() << ")" << std::endl;
		return;
	}
	// The read end must stay with the parent only.
	SetHandleInformation(childStdOutRead, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = childStdOutWrite;
	si.hStdError = childStdOutWrite;
	si.hStdInput = nullptr;
	ZeroMemory(&pi, sizeof(pi));

	std::string commandLine = programPath + " " + arguments;
	std::vector<char> cmdBuf(commandLine.begin(), commandLine.end());
	cmdBuf.push_back('\0');

	const BOOL created = CreateProcessA(
		programPath.c_str(),   // Program path
		cmdBuf.data(),         // Command line (mutable buffer)
		nullptr,               // Process handle not inheritable
		nullptr,               // Thread handle not inheritable
		TRUE,                  // Inherit handles (needed for the stdout pipe)
		CREATE_NO_WINDOW,      // No console window; the role opens its own GUI if windowed
		nullptr,               // Parent's environment block
		nullptr,               // Parent's starting directory
		&si,
		&pi);

	// Close the parent's copy of the write end so ReadFile unblocks on child exit.
	CloseHandle(childStdOutWrite);

	if (!created) {
		std::cerr << tag << "CreateProcess failed (" << GetLastError() << ")" << std::endl;
		CloseHandle(childStdOutRead);
		return;
	}
	CloseHandle(pi.hThread);

	// Forward child output line-by-line, tagged so the launcher can route it.
	// @@STAT lines pass through intact (the launcher strips the leading tag).
	char buffer[4096];
	std::string pending;
	DWORD bytesRead = 0;
	while (ReadFile(childStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
		pending.append(buffer, bytesRead);
		size_t nl;
		while ((nl = pending.find('\n')) != std::string::npos) {
			std::string line = pending.substr(0, nl);
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			std::lock_guard<std::mutex> lock(gStdOutMutex);
			std::cout << tag << line << "\n";
			std::cout.flush();
			pending.erase(0, nl + 1);
		}
	}
	if (!pending.empty()) {
		std::lock_guard<std::mutex> lock(gStdOutMutex);
		std::cout << tag << pending << "\n";
		std::cout.flush();
	}

	CloseHandle(childStdOutRead);
	CloseHandle(pi.hProcess);
}

void ServerMidwareManager::HandleMidwareDataPacket(CSC8503::PhysicsServerMiddlewareDataPacket* packet) {
	mMidwareID = packet->middlewareID;
	std::cout << "Received middleware ID: " << mMidwareID << "\n";
}

void ServerMidwareManager::SendMidwareConnectedPacket(const std::string& ipAddress) {
	PhysicsServerMiddlewareConnectedPacket packet(ipAddress);
	mDistributedManagerClient->SendReliablePacket(packet);
}
