#include <iostream>

#include "SystemManager.h"
#include "../ImguiSceneNode/UINode.h"
#include "Win32Window.h"
#include "../NCLCoreClasses/Window.h"
#include <thread>

#include "DistributedPhysicsManagerServer.h"
#include "GameServer.h"
#include "../CSC8503CoreClasses/NavigationGrid.h"

#include "../NCLCoreClasses/GameTimer.h"

struct ImGuiIO;
using namespace NCL;
using namespace CSC8503;

#include <thread>
#include <ServerStarter.cpp>
#include "../CSC8503CoreClasses/imgui/imgui.h"

namespace {
	constexpr int SYSTEM_MANAGER_PORT = 1234;
	constexpr int MAX_CLIENTS = 4;
	constexpr int MAX_PHYSICS_SERVERS = 2;

	int CREATED_PHYSICS_SERVER_BUFFER = 0;

	constexpr int GAME_AREA_MIN_X = -500;
	constexpr int GAME_AREA_MAX_X = 500;

	constexpr int GAME_AREA_MIN_Z = -500;
	constexpr int GAME_AREA_MAX_Z = 500;
}

DistributedManager::SystemManager* systemManager = nullptr;

struct GameBorder {
	double maxX = 0.f;
	double minX = 0.f;
	double	 minZ = 0.f;
	double maxZ = 0.f;

	GameBorder(double minX, double maxX, double minZ, double maxZ) {
		this->maxX = maxX;
		this->minX = minX;
		this->minZ = minZ;
		this->maxZ = maxZ;
	}
};

GameBorder& CalculateServerBorders(int serverNum) {
	if (serverNum < 0 || serverNum >= MAX_PHYSICS_SERVERS) {
		throw std::out_of_range("Server number out of range");
	}

	GameBorder* border = new GameBorder(0.f, 0.f, 0.f, 0.f);
	double sqrt = 0.f;
	sqrt = std::sqrt(2);
	double ceilVal = std::ceil(sqrt);
	int numCols = static_cast<int>(ceilVal);
	int numRows = static_cast<int>(std::ceil(static_cast<double>(2) / numCols));

	int rectWidth = (GAME_AREA_MAX_X - GAME_AREA_MIN_X) / numCols;
	int rectHeight = (GAME_AREA_MAX_Z - GAME_AREA_MIN_Z) / numRows;

	int row = serverNum / numCols;
	int col = serverNum % numCols;

	border->minX = GAME_AREA_MIN_X + col * rectWidth;
	border->minZ = GAME_AREA_MIN_Z + row * rectHeight;
	border->maxX = (col == numCols - 1) ? GAME_AREA_MAX_Z : (border->minX + rectWidth);
	border->maxZ = (row == numRows - 1) ? GAME_AREA_MAX_Z : (border->minZ + rectHeight);

	return *border;
}

std::string GetServerAreaString(const GameBorder& borders) {
	std::stringstream ss;
	ss << borders.minX << "/" << borders.maxX << "|" << borders.minZ << "/" << borders.maxZ;
	return ss.str();
}

void SetUpPCInputDevices(Window* w) {
	w->ShowOSPointer(false);
	w->LockMouseToWindow(!false);
}

NCL::DistributedManager::DistributedPhysicsServerData* createPhysicsServerData(const std::string& ipAddress, int serverId) {
	NCL::DistributedManager::DistributedPhysicsServerData* data = new NCL::DistributedManager::DistributedPhysicsServerData();
	data->ipAddress = ipAddress;
	data->serverId = serverId;

	return data;
}

void executeProgram(const std::string& programPath, const std::string& arguments, int serverId) {
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;

	//TODO(erendgrmnc): for starting local instance. This block will replace
	const std::string& testServerIp = "127.0.0.1";
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	// Create the command line string
	std::string commandLine = programPath + " " + arguments;

	// Convert the command line string to a mutable C-string
	char* commandLineCStr = const_cast<char*>(commandLine.c_str());

	//Debug
	std::cout << "Command Line arguments: " << commandLineCStr << std::endl;
	auto* serverData = createPhysicsServerData(testServerIp, serverId);

	// Start the independent process.
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
		return;
	}
	else {
		serverData->isServerStarted = true;
		if (systemManager) {
			systemManager->AddServerData(*serverData);
		}
	}

	// Close process and thread handles.
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
}

void StartInstance(int port) {
	std::string programPath = "C:\\Users\\erenl\\Desktop\\GameServer\\EntryPoint.exe"; // Replace with actual path

	int physicsServerId = CREATED_PHYSICS_SERVER_BUFFER++;

	GameBorder borders = CalculateServerBorders(physicsServerId);

	std::string serverBordersStr = GetServerAreaString(borders);

	std::string arguments = "--arg1 127.0.0.1-" + std::to_string(port) + "-" + std::to_string(physicsServerId) + "-" + serverBordersStr;

	std::cout << arguments << std::endl;
	std::thread programThread(executeProgram, programPath, arguments, physicsServerId);

	programThread.detach();
}


void StartManagerServer() {
	//TODO(erendgrmnc): Open UDP server
}

void ConnectClient() {
	//TODO(erendgrmnc): Connect 
}

int ServerUpdater(DistributedManager::SystemManager& systemManager) {
	bool isServerRunning = true;
	while (isServerRunning) {
		systemManager.GetServer()->UpdateServer();
	}

	return 0;
}

int StartProgram() {

	std::cout << "-------------------------- Distributed Manager --------------------------\n";
	systemManager = new DistributedManager::SystemManager(MAX_PHYSICS_SERVERS);

	std::cout << "Starting server on port: " << SYSTEM_MANAGER_PORT << "\n";
	systemManager->StartManagerServer(SYSTEM_MANAGER_PORT, MAX_PHYSICS_SERVERS + MAX_CLIENTS);

	float winWidth = 800;
	float winHeight = 600;

	Window* w = nullptr;
	w = Window::CreateGameWindow("Distributed Game Server Manager", winWidth, winHeight, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);

	w->GetTimer().GetTimeDeltaSeconds(); //Clear the timer so we don't get a larget first dt!
	while (w->UpdateWindow()) {

		if (Window::GetKeyboard()->KeyPressed(KeyCodes::PRIOR)) {
			w->ShowConsole(true);
		}
		if (Window::GetKeyboard()->KeyPressed(KeyCodes::NEXT)) {
			w->ShowConsole(false);
		}

		if (Window::GetKeyboard()->KeyPressed(KeyCodes::T)) {
			w->SetWindowPosition(0, 0);
		}

		if (Window::GetKeyboard()->KeyPressed(KeyCodes::S)) {
			StartInstance(1234);
		}

		if (Window::GetKeyboard()->KeyPressed(KeyCodes::SPACE)) {
			StringPacket packet("Eren");
			systemManager->GetServer()->SendGlobalPacket(packet);
			std::cout << "Packet Sent from Server Manager..." << "\n";
		}

		systemManager->GetServer()->UpdateServer();
	}


	Window::DestroyGameWindow();

	delete systemManager;
	return 0;
}
