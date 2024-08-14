
#include "GameClient.h"
#include "Profiler.h"
#include "ProfilerRenderer.h"
#include "Window.h"

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
	//auto* serverData = createPhysicsServerData(testServerIp, serverId);

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
		//TODO(erendgrmnc): Send packet to server manager that instance is started.
	}

	// Close process and thread handles.
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
}

//TODO(erendgrmnc): Add a data class to receive to be started physics server instance data.
void StartInstance(int port, int physicsServerID, std::string& borderStr) {
	std::string programPath = "C:\\Users\\erenl\\Desktop\\GameServer\\EntryPoint.exe"; // Replace with actual path

	int physicsServerId = physicsServerID;

	const std::string& serverBordersStr = borderStr;

	std::string arguments = "--arg1 127.0.0.1-" + std::to_string(port) + "-" + std::to_string(physicsServerId) + "-" + serverBordersStr;

	std::cout << arguments << std::endl;
	std::thread programThread(executeProgram, programPath, arguments, physicsServerId);

	programThread.detach();
}

int StartMidware() {

	//TODO(erendgrmnc): start the client to connect server manager

	float winWidth = 400;
	float winHeight = 700;

	NCL::Window* w = nullptr;
	w = NCL::Window::CreateGameWindow("Distributed Game Server Manager", winWidth, winHeight, false);
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

		if (Window::GetKeyboard()->KeyPressed(KeyCodes::S)) {
			//StartInstance(1234);
		}


		Profiler::Update();
		profilerRenderer->Render();
	}


	Window::DestroyGameWindow();

	return 0;
}
