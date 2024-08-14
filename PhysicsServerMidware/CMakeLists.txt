set(PROJECT_NAME PhysicsServerMidware)

include("CMakePC.cmake")

# PC CMake file
if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    Create_PC_Physics_Server_Midware_Files()
endif()