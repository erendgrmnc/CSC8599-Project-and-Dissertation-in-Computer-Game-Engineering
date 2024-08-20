#include "./Profiler.h"

#include <corecrt_io.h>
#include <Psapi.h>
#include <imgui/imgui.h>

#include "AnimationSystem.h"

namespace {
	static constexpr int BYTE_TO_MB = 1048576;
}

DWORDLONG Profiler::sUsedVirtualMemory = 0;
DWORDLONG Profiler::sTotalVirtualMem = 0;
SIZE_T Profiler::sVirtualMemUsedByProgram = 0;

DWORDLONG Profiler::sTotalPhysMem = 0;
DWORDLONG Profiler::sUsedPhysMem = 0;
SIZE_T Profiler::sPhysMemUsedByProgram = 0;

bool Profiler::sIsConnectedToGameManager = false;

int Profiler::sTotalCreatedObjects = 0;
int Profiler::sObjectsInBorders = 0;
int Profiler::sCreatedPhysicsServerInstance = 0;
int Profiler::sConnectedPhysicsServerCount = 0;
int Profiler::sConnectedGameClients = 0;
int Profiler::sConnectedPhysicsServerMiddlewares = 0;
int Profiler::sStartedGameInstances = 0;

float Profiler::sFramesPerSecond = 0.f;
float Profiler::sTimePassedPerUpdate = 0.f;
float Profiler::sFrameTime = 0.f;
float Profiler::sNetworkTime = 0.f;
float Profiler::sPhysicsTime = 0.f;
float Profiler::sWorldTime = 0.f;
float Profiler::sPhysicsPredictionTime = 0.f;
float Profiler::sLastDeltaSnapshotTime = 0.f;
float Profiler::sLastFullSnapshotTime = 0.f;

NCL::Profiler::Profiler() {

}

NCL::Profiler::~Profiler() {

}

void NCL::Profiler::Update() {
	CalculateMemoryUsage();
	CalculateMemoryUsageByProgram();
}

std::string Profiler::GetVirtualMemoryUsageByProgram() {
	return GetMemoryUsageStr(sVirtualMemUsedByProgram);;
}

std::string Profiler::GetVirtualMemoryUsage() {
	return GetMemoryUsageStr(sUsedVirtualMemory);
}

std::string Profiler::GetTotalVirtualMemory() {
	return GetMemoryUsageStr(sTotalVirtualMem);
}

std::string Profiler::GetPhysicalMemoryUsageByProgram() {
	return GetMemoryUsageStr(sPhysMemUsedByProgram);
}

std::string Profiler::GetPhysicalMemoryUsage() {
	return GetMemoryUsageStr(sUsedPhysMem);
}

std::string Profiler::GetTotalPhysicalMemory() {
	return GetMemoryUsageStr(sTotalPhysMem);
}

std::string Profiler::GetMemoryUsageStr(DWORDLONG memCtr) {
	std::string memStr = std::to_string(memCtr / BYTE_TO_MB);
	memStr = memStr + "MB";
	return memStr;
}

int Profiler::GetTotalObjectsInServer() {
	return sTotalCreatedObjects;
}

void Profiler::SetTotalObjectsInServer(int objCount) {
	sTotalCreatedObjects = objCount;
}

int Profiler::GetConnectedPhysicsServerCount() {
	return sConnectedPhysicsServerCount;
}

void Profiler::SetConnectedServerCount(int serverCount) {
	sConnectedPhysicsServerCount = serverCount;
}

int Profiler::GetConnectedGameClients() {
	return sConnectedGameClients;
}

void Profiler::SetConnectedGameClients(int connectedGameClients) {
	sConnectedGameClients = connectedGameClients;
}

int Profiler::GetStartedGameInstances() {
	return sStartedGameInstances;
}

void Profiler::SetStartedGameInstance(int val) {
	sStartedGameInstances = val;
}

int Profiler::GetConnectedPhysicsServerMiddlewares() {
	return sConnectedPhysicsServerMiddlewares;
}

void Profiler::SetConnectedPhysicsServerMiddlewares(int val) {
	sConnectedPhysicsServerMiddlewares = val;
}

int Profiler::GetCreatedPhysicsServerCount() {
	return sCreatedPhysicsServerInstance;
}

void Profiler::SetCreatedPhysicsServerCount(int createdServerCount) {
	sCreatedPhysicsServerInstance = createdServerCount;
}

int Profiler::GetObjectsOnBorders() {
	return sObjectsInBorders;
}

void Profiler::SetObjectsInBorders(int ObjCount) {
	sObjectsInBorders = ObjCount;
}

float Profiler::GetFramesPerSecond() {
	return sFramesPerSecond;
}

void Profiler::SetFramesPerSecond(float fps) {
	sFramesPerSecond = fps;
}

float Profiler::GetTimePassedPerUpdate() {
	return sTimePassedPerUpdate;
}

void Profiler::SetTimePassedPerUpdate(float timePassedPerUpdate) {
	sTimePassedPerUpdate = timePassedPerUpdate;
}

float Profiler::GetNetworkTime() {
	return sNetworkTime;
}

float Profiler::GetRenderTime() {
	return sFrameTime;
}

void Profiler::SetRenderTime(float renderTime) {
	sFrameTime = renderTime;
}

float Profiler::GetPhysicsTime() {
	return sPhysicsTime;
}

void Profiler::SetPhysicsTime(float time) {
	sPhysicsTime = time;
}

float Profiler::GetWorldTime() {
	return sWorldTime;
}

void Profiler::SetWorldTime(float time) {
	sWorldTime = time;
}

float Profiler::GetPhysicsPredictionTime() {
	return sPhysicsPredictionTime;
}

void Profiler::SetPhysicsPredictionTime(float time) {
	sPhysicsPredictionTime = time;
}

float Profiler::GetLastDeltaSnapshotTime() {
	return sLastDeltaSnapshotTime;
}

void Profiler::SetLastDeltaSnapshotTime(float time) {
	sLastDeltaSnapshotTime = time;
}

float Profiler::GetLastFullSnapshotTime() {
	return sLastFullSnapshotTime;
}

void Profiler::SetLastFullSnapshotTime(float time) {
	sLastFullSnapshotTime = time;
}

bool Profiler::GetIsConnectedToGameManager() {
	return sIsConnectedToGameManager;
}

void Profiler::SetIsConnectedToGameManager(bool isConnectedToGameManager) {
	sIsConnectedToGameManager = isConnectedToGameManager;
}

void Profiler::SetNetworkTime(float networkTime) {
	sNetworkTime = networkTime;
}

void NCL::Profiler::CalculateMemoryUsage() {
	MEMORYSTATUSEX memInfo;
	memInfo.dwLength = sizeof(MEMORYSTATUSEX);
	GlobalMemoryStatusEx(&memInfo);

	sTotalVirtualMem = memInfo.ullTotalPageFile;
	sUsedVirtualMemory = memInfo.ullTotalPageFile - memInfo.ullAvailPageFile;

	sTotalPhysMem = memInfo.ullTotalPhys;
	sUsedPhysMem = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
}

void Profiler::CalculateMemoryUsageByProgram() {
	PROCESS_MEMORY_COUNTERS_EX pmc;
	GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));

	sVirtualMemUsedByProgram = pmc.PrivateUsage;
	sPhysMemUsedByProgram = pmc.WorkingSetSize;
}
