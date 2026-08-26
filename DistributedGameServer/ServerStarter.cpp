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
		// Fault injection, off by default. Deliberately changes handoff timing, so it
		// must stay 0 for any measurement run.
		worldManager->SetHandoffDelayTicks(config.GetInt("--handoff-delay-ticks", 0));
		// Deterministic handoff application, and the atomicity of ownership. The sender
		// releases at senderTick + L and the receiver installs at the same tick, so no
		// tick has the object owned by nobody. 0 keeps the old apply-on-arrival
		// behaviour, where the sender releases on send and ownership is vacant for one
		// network round trip.
		//
		// The default was 0 until 2026-08-26. 8 was DERIVED from a 60-run sweep, not
		// chosen: L in {0,2,4,8,16}, 6 repeats, at 2 and 4 servers
		// (docs/superpowers/results/2026-08-26-D-ownership.md). Three things that sweep
		// found, none of them obvious:
		//
		//  - L is a THRESHOLD, not a dial. L=2 is worse than L=0: 16.7ms is below this
		//    setup's delivery latency, so ~60% of arrivals miss their slot (hoLate 18
		//    and 31) and the transfer degrades to release-on-send PLUS a scheduling
		//    delay. Every repeat sat at ~1776 gap ticks of 1800, against L=0's 87/~1750.
		//    Do not set a small non-zero value "to be safe".
		//  - Above the threshold, every remaining gap is a late arrival, one for one -
		//    hoLate > 0 iff gap > 0 across 18 runs. The residual is delivery tail
		//    latency, not a protocol defect.
		//  - The tail is machine-dependent. L=8 is clean on all 6 repeats at 2 servers;
		//    at 4 servers one repeat of six shows 3 late arrivals and a 10-tick gap,
		//    because 4 servers plus manager, midware and client is 7 processes on 6
		//    cores. That residual is a limit of the single-machine testbed and is
		//    documented as one, not designed around.
		//
		// L is also bounded ABOVE by the halo band: the sender keeps simulating for L
		// ticks after the object leaves its region, so it must still be inside the
		// receiver's band - v_max * L * dt <= halo_width, i.e. L <= 16 at --halo-width
		// 8. 8 keeps a 2x margin; 16 was clean at both server counts but lands the
		// object exactly on the band edge, which no faster workload would survive.
		worldManager->SetHandoffLookaheadTicks(config.GetInt("--handoff-lookahead", 8));
		// Custody: how long to wait for a handoff ack before resending, and how many
		// sends to count as "attempts" before the transfer is merely HELD rather than
		// retried on the attempt counter. Exhausting attempts does NOT take the object
		// back - only an undeliverable resend does (see DecideCustody); a time-gated
		// reclaim duplicated objects under load. 0 retry ticks disables the mechanism
		// and restores the pre-custody behaviour for comparison.
		worldManager->SetCustodyConfig(config.GetInt("--handoff-retry-ticks", 30),
			config.GetInt("--handoff-max-attempts", 3));
		// Separate from the handoff lookahead and much smaller - see the member's
		// comment. Set BEFORE the width, since the width's safety floor is derived
		// from it.
		worldManager->SetHaloLookaheadTicks(config.GetInt("--halo-lookahead", 4));
		// Injected server-to-server link delay. Set BEFORE the width for the same
		// reason as the lookahead: both are terms in the width's safety floor, so a
		// width validated before they are known is validated against the wrong bound.
		//
		// 0/0 is no injection, which is how every measurement before Phase C ran.
		serverManager->SetLinkDelay(config.GetFloat("--link-latency-ms", 0.0f),
			config.GetFloat("--link-jitter-ms", 0.0f),
			static_cast<unsigned int>(config.GetInt("--seed", 1)));
		// 0, so every configuration that predates the halo behaves exactly as before.
		worldManager->SetHaloWidth(config.GetFloat("--halo-width", 0.0f));
		// Parallel physics. 0 (the default) keeps every phase on this thread, so a
		// run without the flag behaves exactly as every earlier measurement did;
		// -1 asks TaskPool for a default derived from the hardware.
		worldManager->SetPhysicsWorkerThreads(config.GetInt("--physics-threads", 0));
		// Load reporting for dynamic rebalancing. 0 disables it, which is how every
		// run before the policy existed behaved.
		worldManager->SetLoadReportInterval(config.GetInt("--rebalance-interval", 0));
		// Reproducible runs need it; production would not. See PublishHaloBand.
		serverManager->SetHaloReliable(config.Has("--halo-reliable"));
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

		// Only meaningful alongside --handoff-lookahead, which interprets a sender's
		// tick number in the receiver's frame. Default 0 keeps the old behaviour.
		runOptions.epochAlignMicros = static_cast<long long>(config.GetInt("--epoch-align-us", 0));

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

		// Drain phase: keep pumping the network, and keep applying scheduled arrivals,
		// after this server's own run has ended.
		//
		// A fixed tick count does NOT mean the servers finish together. An overloaded
		// server takes longer in wall clock for the same ticks, so a lightly loaded
		// peer reaches its last tick first and exits - and everything the busy one
		// hands it afterwards goes to a dead peer and is lost. Measured on a 4,000
		// object cluster: the light server finished 17 s early and 1,008 objects
		// vanished into the gap, with no error anywhere, because from the sender's side
		// ENet had accepted every packet.
		//
		// Draining does not extend the simulation - the world is not stepped - so the
		// measured run is unchanged. It only lets transfers already in flight land and
		// be counted, which is what makes conservation checkable in the one case that
		// matters: a partition bad enough to need rebalancing.
		const double drainSeconds = static_cast<double>(config.GetInt("--drain-seconds", 5));
		if (drainSeconds > 0.0) {
			std::cout << "Draining for " << drainSeconds << "s so in-flight transfers land.\n";
			NCL::GameTimer drainTimer;
			double drained = 0.0;
			while (drained < drainSeconds) {
				// Tick() FIRST. GetTimeDeltaSeconds only reads the last measured delta;
				// without a Tick it returns the same value forever, which for a fresh
				// timer is zero - so this loop never advanced and the server hung
				// instead of draining.
				drainTimer.Tick();
				const float dt = drainTimer.GetTimeDeltaSeconds();
				drained += dt;
				// Network only. The world is deliberately NOT stepped: an arrival is
				// installed by the scheduled-handoff flush, which the world update
				// drives, so that one part is run explicitly below.
				serverManager->UpdateGameServerManager(dt);
				if (auto* worldManager = serverManager->GetServerWorldManager()) {
					worldManager->DrainScheduledArrivals();
				}
			}
		}

		// Locality (I6). Captured before the @@FINAL line rather than read inline,
		// because GetServerWorldManager can return null on a server that never
		// received its start packet - which is exactly the failure mode these
		// numbers are meant to make visible, so it must not crash the report.
		int poolObjects = -1;
		int worldObjects = -1;
		int forwardEntries = -1;
		int haloObjects = -1;
		int pendingReleases = 0;
		int scheduledHandoffs = 0;
		int handoffsResent = 0;
		int handoffsReclaimed = 0;
		int pendingCustody = 0;
		int handoffsClamped = 0;
		int handoffsDuplicate = 0;
		if (auto* worldManager = serverManager->GetServerWorldManager()) {
			// BEFORE anything below reads a counter. The drain above installs
			// arrivals without stepping the world, so Update() - which is where these
			// are normally published to Profiler - did not run during it. Without
			// this, the Profiler-sourced fields on the @@FINAL line below (hoSent,
			// hoRecv, hoFail, hoLate, haloLate, haloAhead) describe the last stepped
			// tick while the locals captured here (objPool, hoCustody, hoSched...)
			// describe the post-drain state, and the line mixes two instants.
			worldManager->PublishCounters();
			worldManager->FlushMetrics();
			poolObjects = worldManager->GetPoolObjectCount();
			worldObjects = worldManager->GetWorldObjectCount();
			forwardEntries = worldManager->GetForwardEntryCount();
			haloObjects = worldManager->GetHaloObjectCount();
			pendingReleases = worldManager->GetPendingReleaseCount();
			scheduledHandoffs = worldManager->GetScheduledHandoffCount();
			handoffsResent = worldManager->GetHandoffsResent();
			handoffsReclaimed = worldManager->GetHandoffsReclaimed();
			pendingCustody = worldManager->GetPendingCustodyCount();
			handoffsClamped = worldManager->GetHandoffsClamped();
			handoffsDuplicate = worldManager->GetHandoffsDuplicate();
		}

		// Read after the world-manager block for the same reason that block exists:
		// a server that never started still reaches this line, and must report
		// zeroes rather than crash.
		const auto netTotals = serverManager->GetNetworkByteTotals();

		// Final totals rather than a 2 Hz sample, so the I4 and I5 invariants can be
		// checked exactly instead of approximately.
		std::cout << "@@FINAL role=server id=" << serverId
			<< " objs=" << Profiler::GetObjectsOnBorders()
			// Pre-seeded objects built by this server at world construction. Only this
			// server's share is meaningful; every server builds the same set, so the
			// conservation check uses one server's value, not a sum.
			<< " objPreseed=" << Profiler::GetTotalObjectsInServer()
			// Locality (I6): what this server HOLDS. objs above is what it simulates.
			// Under the pre-seed model these equal the world total on EVERY server,
			// which is the O(world) per-server cost the region-local increment exists
			// to remove. -1 means the world was never built.
			<< " objPool=" << poolObjects
			<< " objWorld=" << worldObjects
			// The forwarding table, reported for the same reason as objPool: it is
			// the other per-server structure that could scale with the world.
			<< " objFwd=" << forwardEntries
			// Watched, not simulated. Kept out of objPool so the locality figure keeps
			// meaning what it meant before the halo existed.
			<< " objHalo=" << haloObjects
			// Run total, not a tick sample. Summed across servers and compared with
			// the same world on one server, this is the size of the cross-border
			// collision gap.
			<< " contacts=" << Profiler::GetContactsResolvedTotal()
			<< " hoSent=" << Profiler::GetHandoffsSent()
			<< " hoRecv=" << Profiler::GetHandoffsReceived()
			<< " hoFail=" << Profiler::GetHandoffsFailed()
			<< " hoLate=" << Profiler::GetHandoffsLate()
			<< " hoResent=" << handoffsResent
			<< " hoReclaimed=" << handoffsReclaimed
			<< " hoCustody=" << pendingCustody
			<< " hoClamp=" << handoffsClamped
			// Redundant custody resends that arrived after the object was already
			// installed here, accepted and acked but otherwise ignored. Kept OUT of
			// hoRecv on purpose - hoSent does not count a resend either, so folding
			// these in would break handoff parity (I5).
			<< " hoDup=" << handoffsDuplicate
			// Transfers started but not yet released. hoSent counts the start and
			// hoRecv the completion, so a run ending mid-transfer is short by this
			// many and the parity check has to allow for it.
			<< " hoPending=" << pendingReleases
			// Arrived but not yet installed. Counted separately from hoPending: one is
			// the sender still holding the object, the other the receiver waiting for
			// the agreed tick, and handoff parity has to allow for both.
			<< " hoSched=" << scheduledHandoffs
			<< " cmdApplied=" << Profiler::GetCommandsApplied()
			<< " cmdRelayed=" << Profiler::GetCommandsRelayed()
			<< " cmdDup=" << Profiler::GetCommandsDuplicate()
			<< " cmdRejected=" << Profiler::GetCommandsRejected()
			<< " cmdFanout=" << Profiler::GetCommandsFannedOut()
			<< " objSpawned=" << Profiler::GetObjectsSpawned()
			<< " objDestroyed=" << Profiler::GetObjectsDestroyed()
			<< " manifestSent=" << Profiler::GetManifestEntriesSent()
			// Halo traffic. haloObjSent/haloObjRecv are what the bandwidth claim rests
			// on; the packet counts show how well the batching is working.
			<< " haloSent=" << Profiler::GetHaloUpdatesSent()
			<< " haloObjSent=" << Profiler::GetHaloObjectsSent()
			<< " haloRecv=" << Profiler::GetHaloUpdatesReceived()
			<< " haloObjRecv=" << Profiler::GetHaloObjectsReceived()
			<< " haloLate=" << Profiler::GetHaloUpdatesLate()
			<< " haloAhead=" << Profiler::GetHaloUpdatesAhead()
			// Object-snapshots sent, and the ones a client's declared interest
			// suppressed. This pair is the interest-management result.
			<< " snapSent=" << Profiler::GetSnapshotsSent()
			<< " snapSupp=" << Profiler::GetSnapshotsSuppressed()
			// Real wire cost, not a model. ENet counts these after coalescing
			// commands into datagrams, so E8 no longer has to assume one datagram
			// per packet. Split by host family: client-facing carries snapshots,
			// peer-facing carries halo and handoffs. Header overhead is NOT added
			// here - analyse.py owns that arithmetic, because which headers to
			// charge is an analysis choice rather than a runtime fact.
			<< " netCliBytes=" << netTotals.clientBytes
			<< " netCliPkts=" << netTotals.clientPackets
			<< " netPeerBytes=" << netTotals.peerBytes
			<< " netPeerPkts=" << netTotals.peerPackets
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
