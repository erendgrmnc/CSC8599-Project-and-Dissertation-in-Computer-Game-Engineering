#include <iostream>

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
	// Flush stdout after every insertion. Roles log with `std::cout << ... << "\n"`,
	// which only reaches the pipe when the buffer fills or TelemetryReporter's periodic
	// std::endl flushes it. If a role dies in between, those lines are lost - so the
	// launcher's last visible line is the last @@STAT, not the last thing the process
	// actually did. That made a crash look like it happened somewhere it didn't.
	std::cout << std::unitbuf;

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
