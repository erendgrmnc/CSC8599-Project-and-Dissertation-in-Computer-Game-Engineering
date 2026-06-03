#include <iostream>
#include <string>

#include "DistributedMultiplayerGameScene.h"
#include "Window.h"
#include "Profiler.h"
#include "ProfilerRenderer.h"
#include "DistributedSystemCommonFiles/DistributedUtils.h"
#include "DistributedSystemCommonFiles/LaunchConfig.h"
#include "DistributedSystemCommonFiles/HeadlessRunner.h"

using namespace NCL;

namespace {
	constexpr int DEFAULT_MANAGER_PORT = 1234;
}

// Minimal host for the thin distributed-physics client. Mirrors the server
// roles' ProgramStart.cpp pattern: a window + profiler + an update loop that
// pumps the network clients. No SceneManager / LevelManager / renderer.
int RunDistributedClient(int argc, char* argv[]) {

	const NCL::LaunchConfig config(argc, argv);

	std::cout << "-------------- DISTRIBUTED PHYSICS CLIENT -----------------\n";

	std::string managerIpAddress = "127.0.0.1";
	int managerPort = DEFAULT_MANAGER_PORT;
	int gameInstanceId = 1;

	if (config.HasAnyFlags()) {
		managerIpAddress = config.GetString("--manager-ip", managerIpAddress);
		managerPort = config.GetInt("--manager-port", managerPort);
		gameInstanceId = config.GetInt("--game-instance", gameInstanceId);
		std::cout << "Launch config: manager-ip=" << managerIpAddress << " manager-port=" << managerPort
			<< " game-instance=" << gameInstanceId << "\n";
	}
	else {
		std::cout << "Please enter distributed manager ip address in format: 127.0.0.1\n";
		std::string input;
		std::cin >> input;
		if (input != "e") {
			managerIpAddress = input;
		}

		std::cout << "Please enter distributed manager port (default " << DEFAULT_MANAGER_PORT << "): ";
		std::cin >> managerPort;
	}

	auto* scene = new DistributedMultiplayerGameScene();
	scene->SetGameInstanceId(gameInstanceId);

	const std::vector<char> ipOctets = NCL::DistributedUtils::ConvertIpStrToCharArr(managerIpAddress);
	std::cout << "Connecting to distributed manager on " << managerIpAddress << ":" << managerPort << "\n";
	scene->ConnectClientToDistributedManager(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], managerPort);

	const bool headless = config.Has("--headless");
	auto tick = [&](float dt) {
		scene->UpdateGame(dt);
		Profiler::Update();
	};

	if (headless) {
		std::cout << "Running headless (client).\n";
		NCL::RunHeadlessLoop(tick);
		delete scene;
		return 0;
	}

	Window* w = Window::CreateGameWindow("Distributed Physics Client", 400, 700, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);

	ProfilerRenderer* profilerRenderer = new ProfilerRenderer(*w, ProfilerType::DistributedClient);

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

		tick(w->GetTimer().GetTimeDeltaSeconds());

		profilerRenderer->Render();
	}

	delete scene;
	Window::DestroyGameWindow();

	return 0;
}
