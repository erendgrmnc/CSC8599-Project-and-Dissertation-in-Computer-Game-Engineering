set(PROJECT_NAME DistributedGameServer)

include("CMakePC.cmake")

# PC CMake file
if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    Create_PC_Distributed_Game_Server_Files()
endif()