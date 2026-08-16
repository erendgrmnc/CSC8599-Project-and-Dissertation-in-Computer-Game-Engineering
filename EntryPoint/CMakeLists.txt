set(PROJECT_NAME EntryPoint)

include("CMakePS5.cmake")
include("CMakePC.cmake")
include("CMakeDistributedRoles.cmake")

# The three per-role includes below are superseded by CMakeDistributedRoles.cmake,
# which builds all three server roles in a single configure. Kept for reference; they
# each produced one "EntryPoint" target and so required one configure per role.
#include("CMakeDistributedServerManager.cmake")
#include("CMakeDistributedGameServer.cmake")
#include("CMakePhysicsServerMidware.cmake")

# PC CMake file
if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    if("${CMAKE_DISTRIBUTED_SYSTEM_ACTIVE}" STREQUAL "true")
        # Manager, Midware and Game Server all at once: they share every library and
        # differ only by a compile definition consumed in main.cpp.
        Create_All_Distributed_Role_EntryPoints()
    else()
        # The client flips DISTRIBUTEDSYSTEMACTIVE, which changes the libraries
        # themselves, so it still needs its own configure.
        Create_PC_EntryPoint_Files()
    endif()
endif()

# PS5 Cmake file

if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Prospero")
    Create_PS5_EntryPoint_Files()
endif()