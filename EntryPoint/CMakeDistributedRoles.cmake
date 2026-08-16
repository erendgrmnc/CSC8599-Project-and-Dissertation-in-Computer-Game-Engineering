# Builds every distributed server role in ONE configure.
#
# Why this exists: the three server roles (Manager / Midware / Game Server) are
# selected by BUILDFORDISTRIBUTEDMANAGER and BUILDFORPHYSICSMIDWARE, and those two
# defines appear in exactly one file - EntryPoint/main.cpp - where they choose which
# ProgramStart.cpp gets #included. Every library is byte-identical across the three.
#
# They used to be global add_compile_definitions, so producing the three executables
# meant three full solution rebuilds (with a deleted CMakeCache and a fresh configure
# each time) to obtain three binaries that differ by one #include. Making them
# per-target definitions collapses that to one configure and one build.
#
# The Client is NOT built here: it is the only role that flips
# DISTRIBUTEDSYSTEMACTIVE, which genuinely changes 16 library files, so it still
# needs its own configure. Four builds -> two.

function(Add_Distributed_Role_EntryPoint TARGET_NAME ROLE_DEFINE ROLE_LIBRARY)
    message("Entry Point role target: ${TARGET_NAME} (define='${ROLE_DEFINE}' lib='${ROLE_LIBRARY}')")

    add_executable(${TARGET_NAME} "main.cpp")

    set_target_properties(${TARGET_NAME} PROPERTIES
        VS_GLOBAL_KEYWORD "Win32Proj"
    )
    set_target_properties(${TARGET_NAME} PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE "TRUE"
    )

    ################################################################################
    # Compile definitions - the role selector is per-target, not global
    ################################################################################
    if(MSVC)
        target_compile_definitions(${TARGET_NAME} PRIVATE
            "UNICODE;"
            "_UNICODE"
            "WIN32_LEAN_AND_MEAN"
            "_WINSOCKAPI_"
            "_WINSOCK2API_"
            "_WINSOCK_DEPRECATED_NO_WARNINGS"
        )
    endif()

    # Empty for the game server: main.cpp treats "neither manager nor midware" as the
    # game server case, so that role is selected by the absence of a define.
    if(NOT "${ROLE_DEFINE}" STREQUAL "")
        target_compile_definitions(${TARGET_NAME} PRIVATE "${ROLE_DEFINE}")
    endif()

    target_precompile_headers(${TARGET_NAME} PRIVATE
        <vector>
        <map>
        <stack>
        <list>
        <set>
        <string>
        <thread>
        <atomic>
        <functional>
        <iostream>
        <chrono>
        <sstream>

        "../NCLCoreClasses/Vector2i.h"
        "../NCLCoreClasses/Vector3i.h"
        "../NCLCoreClasses/Vector4i.h"

        "../NCLCoreClasses/Vector2.h"
        "../NCLCoreClasses/Vector3.h"
        "../NCLCoreClasses/Vector4.h"
        "../NCLCoreClasses/Quaternion.h"
        "../NCLCoreClasses/Plane.h"
        "../NCLCoreClasses/Matrix2.h"
        "../NCLCoreClasses/Matrix3.h"
        "../NCLCoreClasses/Matrix4.h"

        "../NCLCoreClasses/GameTimer.h"
    )

    ################################################################################
    # Compile and link options
    ################################################################################
    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE
            $<$<CONFIG:Release>:
                /Oi;
                /Gy
            >
            /permissive-;
            /std:c++latest;
            /sdl;
            /W3;
            ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
            ${DEFAULT_CXX_EXCEPTION_HANDLING};
            /Y-
        )
        target_link_options(${TARGET_NAME} PRIVATE
            $<$<CONFIG:Release>:
                /OPT:REF;
                /OPT:ICF
            >
        )
        target_link_libraries(${TARGET_NAME} LINK_PUBLIC "Winmm.lib")
    endif()

    ################################################################################
    # Dependencies - identical across all three roles apart from the role library
    ################################################################################
    target_link_libraries(${TARGET_NAME} LINK_PUBLIC ${ROLE_LIBRARY})
    target_link_libraries(${TARGET_NAME} LINK_PUBLIC NCLCoreClasses)
    target_link_libraries(${TARGET_NAME} LINK_PUBLIC CSC8503CoreClasses)
    target_link_libraries(${TARGET_NAME} LINK_PUBLIC OpenGLRendering)
    target_link_libraries(${TARGET_NAME} LINK_PUBLIC Recast)
    target_link_libraries(${TARGET_NAME} LINK_PUBLIC Detour)
    target_link_libraries(${TARGET_NAME} LINK_PUBLIC DebugUtils)
    target_link_libraries(${TARGET_NAME} LINK_PUBLIC DetourTileCache)
endfunction()

function(Create_All_Distributed_Role_EntryPoints)
    # include_directories is directory-scoped, so it applies to all three targets.
    include_directories("../DistributedPhysicsManager/")
    include_directories("../PhysicsServerMidware/")
    include_directories("../DistributedGameServer/")
    include_directories("../OpenGLRendering/")
    include_directories("../NCLCoreClasses/")
    include_directories("../CSC8503CoreClasses/")
    include_directories("../Recast")
    include_directories("../Detour")
    include_directories("../DebugUtils")
    include_directories("../DetourTileCache")

    # The manager also links DistributedGameServer, matching the previous
    # CMakeDistributedServerManager.cmake link set.
    Add_Distributed_Role_EntryPoint(EntryPointManager "BUILDFORDISTRIBUTEDMANAGER" DistributedPhysicsManager)
    target_link_libraries(EntryPointManager LINK_PUBLIC DistributedGameServer)

    Add_Distributed_Role_EntryPoint(EntryPointMidware "BUILDFORPHYSICSMIDWARE" PhysicsServerMidware)

    # No role define: main.cpp falls through to the game server.
    Add_Distributed_Role_EntryPoint(EntryPointServer "" DistributedGameServer)
endfunction()
