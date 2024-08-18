set(PROJECT_NAME EntryPoint)

include("CMakePS5.cmake")
include("CMakePC.cmake")
include("CMakeDistributedServerManager.cmake")
include("CMakeDistributedGameServer.cmake")
include("CMakePhysicsServerMidware.cmake")

# PC CMake file
if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    if("${CMAKE_DISTRIBUTED_SYSTEM_ACTIVE}" STREQUAL "true")
        if("${CMAKE_BUILD_FOR_DISTRIBUTED_MANAGER}" STREQUAL "true")
            Create_Distributed_Server_Manager_EntryPoint_Files()
        elseif("${CMAKE_BUILD_FOR_PHYSICS_MIDWARE}" STREQUAL "true")
            Create_Physics_Server_Midware_EntryPoint_Files()
        else()
            Create_Distributed_Game_Server_EntryPoint_Files()
        endif()
    else()
        Create_PC_EntryPoint_Files()
    endif()
endif()

# PS5 Cmake file

if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Prospero")
    Create_PS5_EntryPoint_Files()
endif()