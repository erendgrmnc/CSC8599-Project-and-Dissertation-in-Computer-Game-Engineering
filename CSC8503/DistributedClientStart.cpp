#include <iostream>
#include <string>

#include "DistributedMultiplayerGameScene.h"
#include "Window.h"
#include "Profiler.h"
#include "ProfilerRenderer.h"
#include "DistributedSystemCommonFiles/DistributedUtils.h"
#include "DistributedSystemCommonFiles/LaunchConfig.h"
#include "DistributedSystemCommonFiles/HeadlessRunner.h"
#include "DistributedSystemCommonFiles/TelemetryReporter.h"

#ifndef DISTRIBUTEDSYSTEMACTIVE
#include "DistributedClientRenderer.h"
#include "GameTechRenderer.h"
#include "GameWorld.h"
#include "DirectionalLight.h"
#endif

using namespace NCL;
using namespace NCL::Maths;

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
	NCL::TelemetryReporter reporter(NCL::TelemetryRole::Client);
	auto tick = [&](float dt) {
		scene->UpdateGame(dt);
		Profiler::Update();
		reporter.MaybeEmit(scene->IsGameStarted());
	};

	if (headless) {
		std::cout << "Running headless (client).\n";
		NCL::RunHeadlessLoop(tick);
		delete scene;
		return 0;
	}

#ifndef DISTRIBUTEDSYSTEMACTIVE
	using namespace NCL::CSC8503;
	using namespace NCL::Rendering;

	Window* w = Window::CreateGameWindow("Distributed Physics Client", 1280, 720, false);
	w->ShowOSPointer(false);
	w->LockMouseToWindow(true);

	GameWorld* world = new GameWorld();

	// Overview camera looking at the shared world; free-look with WASD + mouse.
	auto& cam = world->GetMainCamera();
	cam.SetNearPlane(0.1f);
	cam.SetFarPlane(2000.0f);
	cam.SetPitch(-35.0f);
	cam.SetYaw(0.0f);
	cam.SetPosition(Vector3(0, 220, 260));

	// Default: a minimal, safe forward renderer (flat-shaded cubes). The full
	// deferred GameTechRenderer is EXPERIMENTAL and opt-in via --render-deferred:
	// driven without a level it can issue invalid GPU work that hangs the display.
	const bool useDeferred = config.Has("--render-deferred");

	auto runLoop = [&](OGLRenderer* renderer) {
		w->GetTimer().GetTimeDeltaSeconds(); //Clear the timer so we don't get a larger first dt!
		while (w->UpdateWindow()) {
			const float dt = w->GetTimer().GetTimeDeltaSeconds();
			if (Window::GetKeyboard()->KeyPressed(KeyCodes::ESCAPE)) {
				break;
			}
			try {
				scene->UpdateGame(dt);   // pump network clients -> snapshots applied to object transforms
				world->UpdateWorld(dt);
				cam.UpdateCamera(dt);
				Profiler::Update();
				reporter.MaybeEmit(scene->IsGameStarted());
				renderer->Render();
			}
			catch (const std::exception& e) {
				std::cerr << "Client frame exception: " << e.what() << std::endl;
			}
		}
	};

	if (useDeferred) {
		std::cout << "WARNING: --render-deferred uses the EXPERIMENTAL GameTech renderer; it may hang the GPU.\n";
		GameTechRenderer* renderer = new GameTechRenderer(*world);
		auto* cubeMesh = renderer->LoadMesh("Cube.msh");
		auto* albedoTex = renderer->LoadTexture("Default.png");
		auto* normalTex = renderer->LoadTexture("Default.png");
		auto* objShader = renderer->LoadShader("scene.vert", "scene.frag");
		scene->SetRenderResources(world, cubeMesh, albedoTex, normalTex, objShader);
		renderer->FillTextureDataUBO();
		renderer->AddLight(new DirectionLight(Vector3(-0.5f, -1.0f, -0.5f), Vector4(1, 1, 1, 1), 2000.0f, Vector3(0, 0, 0)));
		runLoop(renderer);
		delete renderer;
	}
	else {
		DistributedClientRenderer* renderer = new DistributedClientRenderer(*w, *world);
		scene->SetRenderResources(world, renderer->GetObjectMesh(), nullptr, nullptr, renderer->GetObjectShader());
		runLoop(renderer);
		delete renderer;
	}

	delete world;
	delete scene;
	Window::DestroyGameWindow();
	return 0;
#else
	// RunDistributedClient is only invoked in the non-distributed build; this
	// branch just keeps the file compilable for the lean server configs.
	(void)reporter;
	delete scene;
	return 0;
#endif
}
