set(PROJECT_NAME ImguiSceneNode)

include("CMakePC.cmake")

# PC CMake file
if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
    Create_PC_Imgui_Scene_Node()
endif()