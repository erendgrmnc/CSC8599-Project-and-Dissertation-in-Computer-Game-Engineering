#include "./Profiler.h"

#include <corecrt_io.h>
#include <Psapi.h>
#include <imgui/imgui.h>

using namespace NCL;

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

int Profiler::sHandoffsSent = 0;
int Profiler::sHandoffsReceived = 0;
int Profiler::sHandoffsFailed = 0;
int Profiler::sHandoffsLate = 0;
int Profiler::sCommandsApplied = 0;
int Profiler::sCommandsRelayed = 0;
int Profiler::sCommandsDuplicate = 0;
int Profiler::sCommandsRejected = 0;
int Profiler::sCommandsSent = 0;
int Profiler::sCommandsFannedOut = 0;
int Profiler::sObjectsSpawned = 0;
int Profiler::sObjectsDestroyed = 0;
int Profiler::sManifestEntriesSent = 0;
int Profiler::sIntegratedObjects = 0;
int Profiler::sContactsResolved = 0;
long long Profiler::sContactsResolvedTotal = 0;
int Profiler::sHaloUpdatesSent = 0;
int Profiler::sHaloObjectsSent = 0;
int Profiler::sHaloUpdatesReceived = 0;
int Profiler::sHaloObjectsReceived = 0;
int Profiler::sHaloUpdatesLate = 0;
long long Profiler::sSnapshotsSent = 0;
long long Profiler::sSnapshotsSuppressed = 0;
int Profiler::sDeltasApplied = 0;
int Profiler::sDeltasRejected = 0;
int Profiler::sFullsApplied = 0;

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

int Profiler::GetHandoffsSent() {
	return sHandoffsSent;
}

void Profiler::SetHandoffsSent(int count) {
	sHandoffsSent = count;
}

int Profiler::GetHandoffsReceived() {
	return sHandoffsReceived;
}

void Profiler::SetHandoffsReceived(int count) {
	sHandoffsReceived = count;
}

int Profiler::GetHandoffsFailed() {
	return sHandoffsFailed;
}

void Profiler::SetHandoffsFailed(int count) {
	sHandoffsFailed = count;
}

int Profiler::GetHandoffsLate() {
	return sHandoffsLate;
}

void Profiler::SetHandoffsLate(int count) {
	sHandoffsLate = count;
}

int Profiler::GetCommandsApplied() {
	return sCommandsApplied;
}

void Profiler::SetCommandsApplied(int count) {
	sCommandsApplied = count;
}

int Profiler::GetCommandsRelayed() {
	return sCommandsRelayed;
}

void Profiler::SetCommandsRelayed(int count) {
	sCommandsRelayed = count;
}

int Profiler::GetCommandsDuplicate() {
	return sCommandsDuplicate;
}

void Profiler::SetCommandsDuplicate(int count) {
	sCommandsDuplicate = count;
}

int Profiler::GetCommandsRejected() {
	return sCommandsRejected;
}

void Profiler::SetCommandsRejected(int count) {
	sCommandsRejected = count;
}

int Profiler::GetCommandsSent() {
	return sCommandsSent;
}

void Profiler::SetCommandsSent(int count) {
	sCommandsSent = count;
}

int Profiler::GetCommandsFannedOut() {
	return sCommandsFannedOut;
}

void Profiler::SetCommandsFannedOut(int count) {
	sCommandsFannedOut = count;
}

int Profiler::GetObjectsSpawned() {
	return sObjectsSpawned;
}

void Profiler::SetObjectsSpawned(int count) {
	sObjectsSpawned = count;
}

int Profiler::GetObjectsDestroyed() {
	return sObjectsDestroyed;
}

void Profiler::SetObjectsDestroyed(int count) {
	sObjectsDestroyed = count;
}

int Profiler::GetManifestEntriesSent() {
	return sManifestEntriesSent;
}

void Profiler::SetManifestEntriesSent(int count) {
	sManifestEntriesSent = count;
}

int Profiler::GetIntegratedObjects() {
	return sIntegratedObjects;
}

long long Profiler::GetSnapshotsSent() { return sSnapshotsSent; }
void Profiler::SetSnapshotsSent(long long count) { sSnapshotsSent = count; }
long long Profiler::GetSnapshotsSuppressed() { return sSnapshotsSuppressed; }
void Profiler::SetSnapshotsSuppressed(long long count) { sSnapshotsSuppressed = count; }
int Profiler::GetHaloUpdatesLate() { return sHaloUpdatesLate; }
void Profiler::SetHaloUpdatesLate(int count) { sHaloUpdatesLate = count; }
int Profiler::GetHaloUpdatesSent() { return sHaloUpdatesSent; }
void Profiler::SetHaloUpdatesSent(int count) { sHaloUpdatesSent = count; }
int Profiler::GetHaloObjectsSent() { return sHaloObjectsSent; }
void Profiler::SetHaloObjectsSent(int count) { sHaloObjectsSent = count; }
int Profiler::GetHaloUpdatesReceived() { return sHaloUpdatesReceived; }
void Profiler::SetHaloUpdatesReceived(int count) { sHaloUpdatesReceived = count; }
int Profiler::GetHaloObjectsReceived() { return sHaloObjectsReceived; }
void Profiler::SetHaloObjectsReceived(int count) { sHaloObjectsReceived = count; }

int Profiler::GetContactsResolved() {
	return sContactsResolved;
}

long long Profiler::GetContactsResolvedTotal() {
	return sContactsResolvedTotal;
}

void Profiler::SetContactsResolved(int count) {
	sContactsResolved = count;
	// Cumulative as well as per-tick: the per-tick figure is what the CSV plots,
	// but the run total is what @@FINAL reports and what the before/after
	// comparison actually rests on.
	sContactsResolvedTotal += count;
}

void Profiler::SetIntegratedObjects(int count) {
	sIntegratedObjects = count;
}

int Profiler::GetDeltasApplied() {
	return sDeltasApplied;
}

int Profiler::GetDeltasRejected() {
	return sDeltasRejected;
}

int Profiler::GetFullsApplied() {
	return sFullsApplied;
}

void Profiler::RecordDeltaApplied() {
	++sDeltasApplied;
}

void Profiler::RecordDeltaRejected() {
	++sDeltasRejected;
}

void Profiler::RecordFullApplied() {
	++sFullsApplied;
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
