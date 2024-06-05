set(PROJECT_NAME DistributedPhysicsManager)

include("CMakePC.cmake")

# PC CMake file
if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    Create_PC_Distributed_Physics_Manager_Files()
endif()