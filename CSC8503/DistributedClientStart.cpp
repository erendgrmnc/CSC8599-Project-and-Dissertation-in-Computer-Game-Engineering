#include <iostream>
#include <string>

#include "DistributedMultiplayerGameScene.h"
#include "Window.h"
#include "Profiler.h"
#include "ProfilerRenderer.h"
#include "DistributedSystemCommonFiles/DistributedUtils.h"

using namespace NCL;

namespace {
	constexpr int DEFAULT_MANAGER_PORT = 1234;
}

// Minimal host for the thin distributed-physics client. Mirrors the server
// roles' ProgramStart.cpp pattern: a window + profiler + an update loop that
// pumps the network clients. No SceneManager / LevelManager / renderer.
int RunDistributedClient() {

	std::cout << "-------------- DISTRIBUTED PHYSICS CLIENT -----------------\n";
	std::cout << "Please enter distributed manager ip address in format: 127.0.0.1\n";

	std::string managerIpAddress = "127.0.0.1";
	std::string input;
	std::cin >> input;
	if (input != "e") {
		managerIpAddress = input;
	}

	std::cout << "Please enter distributed manager port (default " << DEFAULT_MANAGER_PORT << "): ";
	int managerPort = DEFAULT_MANAGER_PORT;
	std::cin >> managerPort;

	float winWidth = 400;
	float winHeight = 700;

	Window* w = Window::CreateGameWindow("Distributed Physics Client", winWidth, winHeight, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);

	ProfilerRenderer* profilerRenderer = new ProfilerRenderer(*w, ProfilerType::DistributedClient);

	auto* scene = new DistributedMultiplayerGameScene();

	const std::vector<char> ipOctets = NCL::DistributedUtils::ConvertIpStrToCharArr(managerIpAddress);
	std::cout << "Connecting to distributed manager on " << managerIpAddress << ":" << managerPort << "\n";
	scene->ConnectClientToDistributedManager(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], managerPort);

	w->GetTimer().GetTimeDeltaSeconds(); //Clear the timer so we don't get a larger first dt!
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

		scene->UpdateGame(w->GetTimer().GetTimeDeltaSeconds());

		Profiler::Update();
		profilerRenderer->Render();
	}

	delete scene;
	Window::DestroyGameWindow();

	return 0;
}
