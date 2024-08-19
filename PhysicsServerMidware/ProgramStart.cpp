#include "ServerMidwareManager.h"
#include "GameClient.h"
#include "Profiler.h"
#include "ProfilerRenderer.h"

#include "Window.h"

int StartMidware() {

	std::cout << "-------------- PHYSICS SERVER MIDDLEWARE -----------------\n";
	std::cout << "Please enter distributed manager ip address in format: 127.0.0.1\n";

	std::string distributedManagerIpAddress = "127.0.0.1";
	std::string input;
	std::cin >> input;

	if (input != "e") {
		distributedManagerIpAddress = input;
	}

	std::cout << "Ip to connect: " << distributedManagerIpAddress << "\n";

	std::cout << "Please enter distributed manager port: ";

	int distributedManagerPort;
	std::cin >> distributedManagerPort;

	float winWidth = 400;
	float winHeight = 700;

	NCL::Window* w = nullptr;
	w = NCL::Window::CreateGameWindow("Physics Server Middleware", winWidth, winHeight, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);

	ProfilerRenderer* profilerRenderer = new ProfilerRenderer(*w, ProfilerType::DistributedPhysicsMidware);

	ServerMidwareManager* midwareManager = new ServerMidwareManager();
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
