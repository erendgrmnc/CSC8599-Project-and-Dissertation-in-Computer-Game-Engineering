#include "TelemetryReporter.h"

#include "Profiler.h"

#include <iostream>
#include <sstream>
#include <iomanip>

using namespace NCL;

namespace {
	constexpr std::chrono::milliseconds kInterval(500);

	std::string F2(float v) {
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(2) << v;
		return ss.str();
	}
}

TelemetryReporter::TelemetryReporter(TelemetryRole role, int id) : mRole(role), mId(id) {
	mLastEmit = std::chrono::steady_clock::now();
}

void TelemetryReporter::MaybeEmit(bool gameStarted) {
	const auto now = std::chrono::steady_clock::now();
	if (!mFirst && (now - mLastEmit) < kInterval) {
		return;
	}
	mFirst = false;
	mLastEmit = now;

	std::ostringstream ss;
	ss << "@@STAT ";
	switch (mRole) {
	case TelemetryRole::Manager:
		ss << "role=manager"
			<< " midwares=" << Profiler::GetConnectedPhysicsServerMiddlewares()
			<< " clients=" << Profiler::GetConnectedGameClients()
			<< " instances=" << Profiler::GetStartedGameInstances();
		break;
	case TelemetryRole::Midware:
		ss << "role=midware id=" << mId
			<< " manager=" << (Profiler::GetIsConnectedToGameManager() ? 1 : 0);
		break;
	case TelemetryRole::GameServer:
		ss << "role=server id=" << mId
			<< " objs=" << Profiler::GetObjectsOnBorders()
			<< " total=" << Profiler::GetTotalObjectsInServer()
			// Validity counters. integ should track objs: if it tracks total instead,
			// this server is integrating the whole world rather than its own region.
			// hoSent/hoRecv summed across servers must balance; hoFail must stay 0.
			<< " integ=" << Profiler::GetIntegratedObjects()
			<< " hoSent=" << Profiler::GetHandoffsSent()
			<< " hoRecv=" << Profiler::GetHandoffsReceived()
			<< " hoFail=" << Profiler::GetHandoffsFailed()
			// Command accounting (I4): cmdApplied + cmdRejected + cmdDup summed over
			// servers must equal the commands clients sent. cmdRelayed is an internal
			// hop, counted apart so it is not charged twice.
			<< " cmdApplied=" << Profiler::GetCommandsApplied()
			<< " cmdRelayed=" << Profiler::GetCommandsRelayed()
			<< " cmdDup=" << Profiler::GetCommandsDuplicate()
			<< " cmdRejected=" << Profiler::GetCommandsRejected()
			<< " phys=" << F2(Profiler::GetPhysicsTime())
			<< " world=" << F2(Profiler::GetWorldTime())
			<< " predict=" << F2(Profiler::GetPhysicsPredictionTime())
			<< " full=" << F2(Profiler::GetLastFullSnapshotTime())
			<< " delta=" << F2(Profiler::GetLastDeltaSnapshotTime())
			<< " game=" << (gameStarted ? 1 : 0);
		break;
	case TelemetryRole::Client:
		ss << "role=client id=" << mId
			<< " fps=" << F2(Profiler::GetFramesPerSecond())
			<< " net=" << F2(Profiler::GetNetworkTime())
			// Snapshot accounting: dOK should climb steadily once a full snapshot has
			// been acknowledged. dOK stuck at 0 while dRej climbs means the delta path
			// is dead - the system's longest-standing defect.
			<< " full=" << Profiler::GetFullsApplied()
			<< " dOK=" << Profiler::GetDeltasApplied()
			<< " dRej=" << Profiler::GetDeltasRejected()
			<< " game=" << (gameStarted ? 1 : 0);
		break;
	}

	// std::endl flushes so the launcher receives each line promptly.
	std::cout << ss.str() << std::endl;
}
