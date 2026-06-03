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

	Window* w = Window::CreateGameWindow("Distributed Physics Client", 1280, 720, false);
	w->ShowOSPointer(false);
	w->LockMouseToWindow(true);

	// Render the received world with the engine renderer: one cube per networked
	// object, positioned by the snapshots the scene applies each tick.
	GameWorld* world = new GameWorld();
	GameTechRenderer* renderer = new GameTechRenderer(*world);

	auto* cubeMesh = renderer->LoadMesh("Cube.msh");
	auto* albedoTex = renderer->LoadTexture("Default.png");
	auto* normalTex = renderer->LoadTexture("Default.png");
	auto* objShader = renderer->LoadShader("scene.vert", "scene.frag");
	scene->SetRenderResources(world, cubeMesh, albedoTex, normalTex, objShader);

	// Refresh the bindless-texture-handle UBO now that the object textures are
	// loaded: the renderer only fills it in its constructor, so textures loaded
	// afterwards would otherwise resolve to a garbage handle and fault the GPU.
	renderer->FillTextureDataUBO();

	// A directional light so the deferred renderer isn't pitch black.
	renderer->AddLight(new DirectionLight(Vector3(-0.5f, -1.0f, -0.5f), Vector4(1, 1, 1, 1), 2000.0f, Vector3(0, 0, 0)));

	// Overview camera; free-look with the usual WASD + mouse.
	auto& cam = world->GetMainCamera();
	cam.SetNearPlane(0.1f);
	cam.SetFarPlane(2000.0f);
	cam.SetPitch(-35.0f);
	cam.SetYaw(0.0f);
	cam.SetPosition(Vector3(0, 220, 260));

	int tracedFrames = 0; // step-trace the first few frames that have a replica, to localise crashes
	w->GetTimer().GetTimeDeltaSeconds(); //Clear the timer so we don't get a larger first dt!
	while (w->UpdateWindow()) {
		const float dt = w->GetTimer().GetTimeDeltaSeconds();

		if (Window::GetKeyboard()->KeyPressed(KeyCodes::ESCAPE)) {
			break;
		}
		if (Window::GetKeyboard()->KeyPressed(KeyCodes::PRIOR)) {
			w->ShowConsole(true);
		}
		if (Window::GetKeyboard()->KeyPressed(KeyCodes::NEXT)) {
			w->ShowConsole(false);
		}

		try {
			scene->UpdateGame(dt);   // pump network clients -> snapshots applied to object transforms

			const bool trace = scene->GetReplicaCount() > 0 && tracedFrames < 6;
			if (trace) { ++tracedFrames; std::cout << "[trace] post-updategame, replicas=" << scene->GetReplicaCount() << std::endl; }

			world->UpdateWorld(dt);  // refresh world bookkeeping
			if (trace) std::cout << "[trace] post-updateworld" << std::endl;

			cam.UpdateCamera(dt);    // free-look
			if (trace) std::cout << "[trace] post-camupdate" << std::endl;

			Profiler::Update();
			reporter.MaybeEmit(scene->IsGameStarted());
			if (trace) std::cout << "[trace] pre-render" << std::endl;

			renderer->Render();
			if (trace) std::cout << "[trace] post-render" << std::endl;
		}
		catch (const std::exception& e) {
			std::cerr << "Client frame exception: " << e.what() << std::endl;
		}
	}

	delete renderer;
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
