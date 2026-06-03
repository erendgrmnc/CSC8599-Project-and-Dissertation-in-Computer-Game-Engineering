#include "ServerMidwareManager.h"
#include "GameClient.h"
#include "Profiler.h"
#include "ProfilerRenderer.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/LaunchConfig.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/HeadlessRunner.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/TelemetryReporter.h"

#include "Window.h"

int StartMidware(int argc, char* argv[]) {

	const NCL::LaunchConfig config(argc, argv);

	std::cout << "-------------- PHYSICS SERVER MIDDLEWARE -----------------\n";

	std::string distributedManagerIpAddress = "127.0.0.1";
	int distributedManagerPort = 1234;
	std::string serverExePath;

	if (config.HasAnyFlags()) {
		distributedManagerIpAddress = config.GetString("--manager-ip", distributedManagerIpAddress);
		distributedManagerPort = config.GetInt("--manager-port", distributedManagerPort);
		serverExePath = config.GetString("--server-exe");
		std::cout << "Launch config: manager-ip=" << distributedManagerIpAddress
			<< " manager-port=" << distributedManagerPort
			<< " server-exe=" << (serverExePath.empty() ? "(default)" : serverExePath) << "\n";
	}
	else {
		std::cout << "Please enter distributed manager ip address in format: 127.0.0.1\n";
		std::string input;
		std::cin >> input;
		if (input != "e") {
			distributedManagerIpAddress = input;
		}
		std::cout << "Ip to connect: " << distributedManagerIpAddress << "\n";
		std::cout << "Please enter distributed manager port: ";
		std::cin >> distributedManagerPort;
	}

	const bool headless = config.Has("--headless");

	ServerMidwareManager* midwareManager = new ServerMidwareManager();
	midwareManager->SetServerExePath(serverExePath);
	midwareManager->SetHeadless(headless);
	midwareManager->ConnectToDistributedManager(distributedManagerIpAddress, distributedManagerPort);

	NCL::TelemetryReporter reporter(NCL::TelemetryRole::Midware);
	auto tick = [&](float dt) {
		midwareManager->Update(dt);
		Profiler::Update();
		reporter.MaybeEmit();
	};

	if (headless) {
		std::cout << "Running headless (midware).\n";
		NCL::RunHeadlessLoop(tick);
		return 0;
	}

	NCL::Window* w = NCL::Window::CreateGameWindow("Physics Server Middleware", 400, 700, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);

	ProfilerRenderer* profilerRenderer = new ProfilerRenderer(*w, ProfilerType::DistributedPhysicsMidware);

	w->GetTimer().GetTimeDeltaSeconds(); //Clear the timer so we don't get a larget first dt!
	while (w->UpdateWindow()) {

		if (NCL::Window::GetKeyboard()->KeyPressed(KeyCodes::PRIOR)) {
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


	Window::DestroyGameWindow();

	return 0;
}
