#ifdef DISTRIBUTEDSYSTEMACTIVE
	#ifdef BUILDFORDISTRIBUTEDMANAGER
	#include "../DistributedPhysicsManager/ProgramStart.cpp"
	#elif BUILDFORPHYSICSMIDWARE
	#include "../PhysicsServerMidware/ProgramStart.cpp"
	#else
	#include "../DistributedGameServer/ServerStarter.cpp"
	#endif
#else
	#include "../CSC8503/DistributedClientStart.cpp"
#endif

#ifdef USEPROSPERO
size_t sceUserMainThreadStackSize = 4 * 1024 * 1024;
extern const char sceUserMainThreadName[] = "TeamProjectGameMain";
int sceUserMainThreadPriority = SCE_KERNEL_PRIO_FIFO_DEFAULT;
size_t sceLibcHeapSize = 257 * 1024 * 1024;
#endif

int main(int argc, char* argv[]) {
#ifdef DISTRIBUTEDSYSTEMACTIVE
	#ifdef BUILDFORDISTRIBUTEDMANAGER
	StartProgram(argc, argv);
	#elif BUILDFORPHYSICSMIDWARE
	StartMidware(argc, argv);
	#else
	StartGameServer(argc, argv);
#endif

#else
	RunDistributedClient(argc, argv);
#endif

}
