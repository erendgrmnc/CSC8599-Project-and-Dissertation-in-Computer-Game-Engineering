#include "ServerMidwareManager.h"
#include "GameClient.h"
#include "Profiler.h"
#include "ProfilerRenderer.h"
#include "../CSC8503CoreClasses/DistributedSystemCommonFiles/LaunchConfig.h"

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

	float winWidth = 400;
	float winHeight = 700;

	NCL::Window* w = nullptr;
	w = NCL::Window::CreateGameWindow("Physics Server Middleware", winWidth, winHeight, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);

	ProfilerRenderer* profilerRenderer = new ProfilerRenderer(*w, ProfilerType::DistributedPhysicsMidware);

	ServerMidwareManager* midwareManager = new ServerMidwareManager();
	midwareManager->SetServerExePath(serverExePath);
	midwareManager->ConnectToDistributedManager(distributedManagerIpAddress, distributedManagerPort);

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

		midwareManager->Update(w->GetTimer().GetTimeDeltaSeconds());

		Profiler::Update();
		profilerRenderer->Render();
	}


	Window::DestroyGameWindow();

	return 0;
}
