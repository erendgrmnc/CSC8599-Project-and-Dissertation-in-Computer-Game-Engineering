#pragma once
#include "AnimationSystem.h"

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

		static int GetTotalObjectsInServer();
		static void SetTotalObjectsInServer(int objCount);

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

	protected:

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
	};
}
