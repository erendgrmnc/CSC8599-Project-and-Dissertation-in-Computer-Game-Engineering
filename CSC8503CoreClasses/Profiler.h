#pragma once

#include <Windows.h>

namespace NCL {
	class Profiler {
	public:
		Profiler();
		~Profiler();
		static void Update();

		static std::string GetVirtualMemoryUsageByProgram();
		static std::string GetVirtualMemoryUsage();
		static std::string GetTotalVirtualMemory();

		static std::string GetPhysicalMemoryUsageByProgram();
		static std::string GetPhysicalMemoryUsage();
		static std::string GetTotalPhysicalMemory();

		static std::string GetMemoryUsageStr(DWORDLONG memCtr);

		static bool GetIsConnectedToGameManager();
		static void SetIsConnectedToGameManager(bool isConnectedToGameManager);

		static int GetTotalObjectsInServer();
		static void SetTotalObjectsInServer(int objCount);

		static int GetConnectedPhysicsServerCount();
		static void SetConnectedServerCount(int serverCount);

		static int GetConnectedGameClients();
		static void SetConnectedGameClients(int connectedGameClients);

		static int GetStartedGameInstances();
		static void SetStartedGameInstance(int val);

		static int GetConnectedPhysicsServerMiddlewares();
		static void SetConnectedPhysicsServerMiddlewares(int val);

		static int GetCreatedPhysicsServerCount();
		static void SetCreatedPhysicsServerCount(int createdServerCount);

		static int GetObjectsOnBorders();
		static void SetObjectsInBorders(int objCount);

		static float GetFramesPerSecond();
		static void SetFramesPerSecond(float fps);

		static float GetTimePassedPerUpdate();
		static void SetTimePassedPerUpdate(float timePassedPerUpdate);

		static float GetNetworkTime();
		static void SetNetworkTime(float networkTime);

		static float GetRenderTime();
		static void SetRenderTime(float renderTime);

		static float GetPhysicsTime();
		static void SetPhysicsTime(float time);

		static float GetWorldTime();
		static void SetWorldTime(float time);

		static float GetPhysicsPredictionTime();
		static void SetPhysicsPredictionTime(float time);

		static float GetLastDeltaSnapshotTime();
		static void SetLastDeltaSnapshotTime(float time);

		static float GetLastFullSnapshotTime();
		static void SetLastFullSnapshotTime(float time);

		// Handoff parity (invariant I5). Summed across all servers, sent should equal
		// received; any gap means objects were lost or duplicated in transit. These
		// are cumulative per-run totals, not rates.
		static int GetHandoffsSent();
		static void SetHandoffsSent(int count);

		static int GetHandoffsReceived();
		static void SetHandoffsReceived(int count);

		static int GetHandoffsFailed();
		static void SetHandoffsFailed(int count);

		// Interaction command accounting (invariant I4). Summed across all servers,
		// commands sent by clients must equal applied + rejected + duplicate; relayed
		// is an internal hop and is counted separately so it is not double-charged.
		// Without these, a silently swallowed command is invisible.
		static int GetCommandsApplied();
		static void SetCommandsApplied(int count);

		static int GetCommandsRelayed();
		static void SetCommandsRelayed(int count);

		static int GetCommandsDuplicate();
		static void SetCommandsDuplicate(int count);

		static int GetCommandsRejected();
		static void SetCommandsRejected(int count);

		// Client side of the same invariant: how many commands this client actually
		// put on the wire.
		static int GetCommandsSent();
		static void SetCommandsSent(int count);

		// Area-effect fan-out hops. One area command legitimately applies once per
		// overlapped region, so I4 only balances after subtracting these from the
		// applied total.
		static int GetCommandsFannedOut();
		static void SetCommandsFannedOut(int count);

		// Runtime spawns originated by this server. Conservation becomes
		// pre-seeded + spawned - destroyed once objects can be created at runtime.
		static int GetObjectsSpawned();
		static void SetObjectsSpawned(int count);

		static int GetObjectsDestroyed();
		static void SetObjectsDestroyed(int count);

		// Late-join manifest entries sent to individual peers.
		static int GetManifestEntriesSent();
		static void SetManifestEntriesSent(int count);

		// Objects this server actually integrated on the last tick, versus the number
		// it owns. These must match: a gap means the integrator is touching objects
		// outside this server's region, which flattens the scaling curve.
		static int GetIntegratedObjects();
		static void SetIntegratedObjects(int count);

		// Client-side snapshot accounting. Deltas silently failing to apply is the
		// system's longest-lived bug, and until these existed nothing anywhere
		// reported whether a delta was used or thrown away. Cumulative per run.
		static int GetDeltasApplied();
		static int GetDeltasRejected();
		static int GetFullsApplied();
		static void RecordDeltaApplied();
		static void RecordDeltaRejected();
		static void RecordFullApplied();

	protected:

		static bool sIsConnectedToGameManager;

		static void CalculateMemoryUsage();
		static void CalculateMemoryUsageByProgram();

		static DWORDLONG sUsedVirtualMemory;
		static DWORDLONG sTotalVirtualMem;
		static SIZE_T sVirtualMemUsedByProgram;

		static DWORDLONG sTotalPhysMem;
		static DWORDLONG sUsedPhysMem;
		static SIZE_T sPhysMemUsedByProgram;

		static int sTotalCreatedObjects;
		static int sObjectsInBorders;
		static int sCreatedPhysicsServerInstance;
		static int sConnectedPhysicsServerCount;
		static int sConnectedPhysicsServerMiddlewares;
		static int sConnectedGameClients;
		static int sStartedGameInstances;

		static float sTimePassedPerUpdate;
		static float sRenderTime;
		static float sFrameTime;
		static float sFramesPerSecond;
		static float sNetworkTime;
		static float sPhysicsTime;
		static float sPhysicsPredictionTime;
		static float sWorldTime;
		static float sLastDeltaSnapshotTime;
		static float sLastFullSnapshotTime;

		static int sHandoffsSent;
		static int sHandoffsReceived;
		static int sHandoffsFailed;
		static int sCommandsApplied;
		static int sCommandsRelayed;
		static int sCommandsDuplicate;
		static int sCommandsRejected;
		static int sCommandsSent;
		static int sCommandsFannedOut;
		static int sObjectsSpawned;
		static int sObjectsDestroyed;
		static int sManifestEntriesSent;
		static int sIntegratedObjects;
		static int sDeltasApplied;
		static int sDeltasRejected;
		static int sFullsApplied;
	};
}
