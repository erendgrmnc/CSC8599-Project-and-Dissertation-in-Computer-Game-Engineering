#include <iostream>

#include "SystemManager.h"
#include "Win32Window.h"
#include "../NCLCoreClasses/Window.h"
#include <thread>

#include "DistributedPhysicsManagerServer.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/DistributedUtils.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/LaunchConfig.h"
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

	// Parse "minX,maxX,minZ,maxZ" into the four world bounds. Leaves the
	// defaults untouched if the string is malformed.
	void ParseWorldBounds(const std::string& worldStr, double& minX, double& maxX, double& minZ, double& maxZ) {
		std::stringstream ss(worldStr);
		std::string token;
		double values[4];
		int count = 0;
		while (count < 4 && std::getline(ss, token, ',')) {
			try {
				values[count] = std::stod(token);
			}
			catch (...) {
				return;
			}
			++count;
		}
		if (count == 4) {
			minX = values[0];
			maxX = values[1];
			minZ = values[2];
			maxZ = values[3];
		}
	}
}

DistributedManager::SystemManager* systemManager = nullptr;

void SetUpPCInputDevices(Window* w) {
	w->ShowOSPointer(false);
	w->LockMouseToWindow(!false);
}

int StartProgram(int argc, char* argv[]) {

	const NCL::LaunchConfig config(argc, argv);
	const bool useFlags = config.HasAnyFlags();

	float winWidth = 400;
	float winHeight = 700;

	Window* w = Window::CreateGameWindow("Distributed Game Server Manager", winWidth, winHeight, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);

	ProfilerRenderer* profilerRenderer = new ProfilerRenderer(*w, ProfilerType::DistributedPhysicsServerManager);

	std::cout << "-------------------------- Distributed Manager --------------------------\n";

	// World area the server regions are carved out of (defaults to the legacy -150..150).
	double worldMinX = -150.0, worldMaxX = 150.0, worldMinZ = -150.0, worldMaxZ = 150.0;

	int maxPhysicsServer = 0;
	int maxClients = 0;
	int objectsPerPlayer = 1;
	int managerPort = SYSTEM_MANAGER_PORT;
	int expectedMidwares = 1;
	bool autoStart = false;

	if (useFlags) {
		// Non-interactive: driven by the GUI launcher / command line.
		maxPhysicsServer = config.GetInt("--servers", 1);
		maxClients = config.GetInt("--clients", 1);
		objectsPerPlayer = config.GetInt("--objects", 1);
		managerPort = config.GetInt("--port", SYSTEM_MANAGER_PORT);
		expectedMidwares = config.GetInt("--midwares", 1);
		autoStart = config.Has("--autostart");
		ParseWorldBounds(config.GetString("--world"), worldMinX, worldMaxX, worldMinZ, worldMaxZ);
		std::cout << "Launch config: servers=" << maxPhysicsServer << " clients=" << maxClients
			<< " objects=" << objectsPerPlayer << " port=" << managerPort
			<< " midwares=" << expectedMidwares << " autostart=" << (autoStart ? "yes" : "no")
			<< " world=" << worldMinX << "," << worldMaxX << "," << worldMinZ << "," << worldMaxZ << "\n";
	}
	else {
		std::cout << "Enter Physics servers to start: ";
		std::cin >> maxPhysicsServer;

		std::cout << "Enter max clients to connect: ";
		std::cin >> maxClients;

		std::cout << "Enter objects to create per player: ";
		std::cin >> objectsPerPlayer;
	}

	systemManager = new DistributedManager::SystemManager(maxPhysicsServer, maxClients);

	std::cout << "Starting server on port: " << managerPort << "\n";
	systemManager->StartManagerServer(managerPort, maxPhysicsServer + maxClients + 20);

	bool instanceCreated = false;


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
			//Start a game instance
			systemManager->CreateNewGameInstance(maxPhysicsServer, maxClients, objectsPerPlayer, worldMinX, worldMaxX, worldMinZ, worldMaxZ);
			instanceCreated = true;
		}

		// --autostart: create the instance automatically once the expected number
		// of midwares have connected, so the launcher needs no key press.
		if (autoStart && !instanceCreated && systemManager->GetConnectedMidwareCount() >= expectedMidwares) {
			std::cout << "Autostart: " << expectedMidwares << " midware(s) connected, creating game instance.\n";
			systemManager->CreateNewGameInstance(maxPhysicsServer, maxClients, objectsPerPlayer, worldMinX, worldMaxX, worldMinZ, worldMaxZ);
			instanceCreated = true;
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
