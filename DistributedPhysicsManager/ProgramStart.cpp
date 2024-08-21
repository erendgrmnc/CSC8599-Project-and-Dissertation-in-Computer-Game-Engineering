#include <iostream>

#include "SystemManager.h"
#include "Win32Window.h"
#include "../NCLCoreClasses/Window.h"
#include <thread>

#include "DistributedPhysicsManagerServer.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/DistributedUtils.h"
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
}

DistributedManager::SystemManager* systemManager = nullptr;

void SetUpPCInputDevices(Window* w) {
	w->ShowOSPointer(false);
	w->LockMouseToWindow(!false);
}

int StartProgram() {

	float winWidth = 400;
	float winHeight = 700;

	Window* w = Window::CreateGameWindow("Distributed Game Server Manager", winWidth, winHeight, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);

	ProfilerRenderer* profilerRenderer = new ProfilerRenderer(*w, ProfilerType::DistributedPhysicsServerManager);

	std::cout << "-------------------------- Distributed Manager --------------------------\n";

	int maxPhysicsServer = 0;
	std::cout << "Enter Physics servers to start: ";
	std::cin >> maxPhysicsServer;

	int maxClients = 0;
	std::cout << "Enter max clients to connect: ";
	std::cin >> maxClients;

	int objectsPerPlayer = 1;
	std::cout << "Enter objects to create per player: ";
	std::cin >> objectsPerPlayer;

	systemManager = new DistributedManager::SystemManager(maxPhysicsServer, maxClients);

	std::cout << "Starting server on port: " << SYSTEM_MANAGER_PORT << "\n";
	systemManager->StartManagerServer(SYSTEM_MANAGER_PORT, maxPhysicsServer + maxClients + 20);


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
			systemManager->CreateNewGameInstance(maxPhysicsServer, maxClients, objectsPerPlayer);
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
