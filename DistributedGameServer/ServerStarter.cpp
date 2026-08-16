#include <iostream>
#include <sstream>

#include "DistributedGameServerManager.h"
#include "GameTimer.h"
#include "Profiler.h"
#include "ProfilerRenderer.h"
#include "ServerWorldManager.h"
#include "DistributedSystemCommonFiles/LaunchConfig.h"
#include "DistributedSystemCommonFiles/HeadlessRunner.h"
#include "DistributedSystemCommonFiles/TelemetryReporter.h"

int ParsePortNumber(std::string& portStr) {
	int port;

	try {
		port = std::stoi(portStr);
		if (port < 0 || port > 65535) {
			throw std::out_of_range("Port number must be between 0 and 65535");
		}
	}
	catch (const std::invalid_argument& e) {
		std::cerr << "Invalid port number: " << portStr << std::endl;
		return 1;
	}
	catch (const std::out_of_range& e) {
		std::cerr << e.what() << std::endl;
		return 1;
	}

	return port;
}

std::vector<char> ipv4_to_char_array(const std::string& ip_str) {
	std::vector<std::string> ip_bytes;
	std::stringstream ss(ip_str);
	std::string segment;

	while (std::getline(ss, segment, '.')) {
		ip_bytes.push_back(segment);
	}

	if (ip_bytes.size() != 4) {
		throw std::invalid_argument("Invalid IPv4 address format");
	}

	std::vector<char> ip_packed;
	for (const std::string& byte_str : ip_bytes) {
		int byte_value = std::stoi(byte_str);
		if (byte_value < 0 || byte_value > 255) {
			throw std::invalid_argument("Invalid IPv4 address format");
		}
		ip_packed.push_back(static_cast<char>(byte_value));
	}

	return ip_packed;
}

int StartGameServer(int argc, char* argv[]) {

	std::cout << "\nCommand-line arguments:\n";
	for (int count = 0; count < argc; count++)
		std::cout << "  argv[" << count << "]   " << argv[count] << "\n";

	//FOR TEST
	bool isTesting = false;
	std::string testArgument = "127.0.0.1-2000-1";

	std::string argument = isTesting ? testArgument : argv[2];

	std::vector<size_t> separators;
	for (size_t pos = 0; (pos = argument.find('-', pos)) != std::string::npos; ++pos) {
		separators.push_back(pos);
	}

	std::string ipAddress = argument.substr(0, separators[0]);
	std::string portStr = argument.substr(separators[0] + 1, separators[1] - separators[0] - 1);
	std::string serverIdStr = argument.substr(separators[1] + 1);
	std::string gameInstanceIdStr = argument.substr(separators[2] + 1);
	std::string serverBorders = argument.substr(separators[3] + 1);

	int serverId = std::stoi(serverIdStr);
	int gameInstanceID = std::stoi(gameInstanceIdStr);

	int port = ParsePortNumber(portStr);

	std::vector<char> ipOctets = ipv4_to_char_array(ipAddress);

	// Now you have parsed IP address (e.g., store in a struct) and port number
	std::cout << "Parsed IP address: " << ipAddress << '\n';
	std::cout << "Parsed port number: " << port << '\n';
	std::cout << "Parsed server ID: " << serverId << '\n';
	std::cout << "Parsed game instance ID: " << gameInstanceID << "\n";
	std::cout << "Parsed server borders string: " << serverBorders << '\n';

	const NCL::LaunchConfig config(argc, argv);

	NCL::DistributedGameServer::DistributedGameServerManager* serverManager = new NCL::DistributedGameServer::DistributedGameServerManager(serverId, gameInstanceID, serverBorders);

	// Determinism settings must be applied before the world is built and before the
	// first physics tick. Every server in an instance must be given the same values
	// or their object sets and trajectories diverge.
	if (auto* worldManager = serverManager->GetServerWorldManager()) {
		const bool fixedStep = config.Has("--fixed-step");
		worldManager->SetFixedTimestep(fixedStep);
		worldManager->SetWorldSeed(static_cast<unsigned int>(config.GetInt("--seed", 1)));
		worldManager->SetWorkload(config.GetString("--workload", ""));
		std::cout << "Determinism: fixed-step=" << (fixedStep ? "on" : "off")
			<< " seed=" << worldManager->GetWorldSeed()
			<< " workload=" << (worldManager->GetWorkload().empty() ? "(none)" : worldManager->GetWorkload())
			<< "\n";

		// Per-tick metrics. One file per server so runs never interleave writes.
		const std::string metricsDir = config.GetString("--metrics-dir", "");
		if (!metricsDir.empty()) {
			const std::string path = metricsDir + "/ticks-server" + std::to_string(serverId) + ".csv";
			worldManager->EnableMetrics(path, static_cast<size_t>(config.GetInt("--metrics-capacity", 200000)));
		}
	}

	serverManager->StartDistributedGameServer(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], port);

	const bool headless = config.Has("--headless");

	NCL::TelemetryReporter reporter(NCL::TelemetryRole::GameServer, serverId);

	// The per-tick work is identical in headless and windowed modes; only the loop
	// host (a GameTimer loop vs a Window) and the profiler overlay differ.
	auto tick = [&](float dt) {
		if (serverManager->GetGameStarted()) {
			serverManager->GetServerWorldManager()->Update(dt);
		}
		serverManager->UpdateGameServerManager(dt);
		Profiler::Update();
		reporter.MaybeEmit(serverManager->GetGameStarted());
	};

	if (headless) {
		// A bounded run is what makes an unattended experiment comparable, and it is
		// the only path on which buffered metrics get written - a force-killed
		// process loses them.
		NCL::HeadlessRunOptions runOptions;
		runOptions.runSeconds = static_cast<double>(config.GetInt("--run-seconds", 0));
		runOptions.runTicks = static_cast<long long>(config.GetInt("--run-ticks", 0));

		// --fixed-step pins the physics substep rate. On its own that is NOT enough
		// for a reproducible run: the loop still feeds Update a measured wall-clock
		// dt, so the tick count over a fixed wall-clock window varies with machine
		// load and per-tick work such as the border check lands at different
		// simulated times. Pinning the loop dt as well is what closes that gap.
		if (config.Has("--fixed-step")) {
			if (auto* worldManager = serverManager->GetServerWorldManager()) {
				runOptions.fixedDt = worldManager->GetFixedTimestepDt();
			}
		}

		// Bootstrap ticks pump the network before the world exists; charging them to
		// the budget would end the run before the first object is ever simulated.
		runOptions.countTicksWhen = [serverManager]() {
			return serverManager->GetGameStarted();
		};

		std::cout << "Running headless (server " << serverId << ")";
		if (runOptions.runTicks > 0) {
			std::cout << " for " << runOptions.runTicks << " ticks";
		}
		else if (runOptions.runSeconds > 0.0) {
			std::cout << " for " << runOptions.runSeconds << "s";
		}
		std::cout << ".\n";

		NCL::RunHeadlessLoop(tick, runOptions);

		if (auto* worldManager = serverManager->GetServerWorldManager()) {
			worldManager->FlushMetrics();
		}

		// Final totals rather than a 2 Hz sample, so the I4 and I5 invariants can be
		// checked exactly instead of approximately.
		std::cout << "@@FINAL role=server id=" << serverId
			<< " objs=" << Profiler::GetObjectsOnBorders()
			<< " hoSent=" << Profiler::GetHandoffsSent()
			<< " hoRecv=" << Profiler::GetHandoffsReceived()
			<< " hoFail=" << Profiler::GetHandoffsFailed()
			<< " cmdApplied=" << Profiler::GetCommandsApplied()
			<< " cmdRelayed=" << Profiler::GetCommandsRelayed()
			<< " cmdDup=" << Profiler::GetCommandsDuplicate()
			<< " cmdRejected=" << Profiler::GetCommandsRejected()
			<< " cmdFanout=" << Profiler::GetCommandsFannedOut()
			<< "\n";
		return 0;
	}

	//PROFILER
	Window* w = Window::CreateGameWindow("Profiler", 400, 700, false);
	w->ShowOSPointer(true);
	w->LockMouseToWindow(false);
	auto* profilerRenderer = new ProfilerRenderer(*w, NCL::ProfilerType::DistributedPhysicsServer);

	NCL::GameTimer timer;
	timer.GetTimeDeltaSeconds(); //Clear the timer so we don't get a larget first dt!
	while (w->UpdateWindow()) {
		timer.Tick();
		float dt = timer.GetTimeDeltaSeconds();
		if (dt > 0.1f) {
			std::cout << "Skipping large time delta" << '\n';
			continue; //must have hit a breakpoint or something to have a 1 second frame time!
		}

		tick(dt);
		profilerRenderer->Render();
	}

	return 0;
}
