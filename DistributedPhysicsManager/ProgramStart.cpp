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

	int CREATED_PHYSICS_SERVER_BUFFER = 0;

}

DistributedManager::SystemManager* systemManager = nullptr;

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

	const std::string& serverBordersStr = systemManager->GetServerAreaString(physicsServerId);

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
	int maxPhysicsServer = 0;
	std::cout << "Enter Physics servers to start: ";
	std::cin >> maxPhysicsServer;

	int maxClients = 0;
	std::cout << "Enter max clients to connect: ";
	std::cin >> maxClients;

	systemManager = new DistributedManager::SystemManager(maxPhysicsServer, maxClients);

	std::cout << "Starting server on port: " << SYSTEM_MANAGER_PORT << "\n";
	systemManager->StartManagerServer(SYSTEM_MANAGER_PORT, maxPhysicsServer + maxClients);

	float winWidth = 400;
	float winHeight = 700;

	Window* w = nullptr;
	w = Window::CreateGameWindow("Distributed Game Server Manager", winWidth, winHeight, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);

	ProfilerRenderer* profilerRenderer = new ProfilerRenderer(*w, ProfilerType::DistributedPhysicsServerManager);

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

		Profiler::Update();
		profilerRenderer->Render();

	}


	Window::DestroyGameWindow();

	delete systemManager;
	return 0;
}
