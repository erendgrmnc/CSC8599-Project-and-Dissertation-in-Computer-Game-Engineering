# Tier 0 unit tests (see the interactions design spec, section 7.3).
#
# Deliberately a separate executable rather than anything linked into a role: it
# must never be able to perturb the four shipped binaries. It links the same
# libraries EntryPointServer does, minus DistributedGameServer, because the
# engine's static libs are interdependent and pull in the GL backend at link time.
#
# Deliberately no project() call: a nested project() makes CMake emit a second,
# stray InteractionTests.sln alongside the real one.

include_directories("../../NCLCoreClasses/")
include_directories("../../CSC8503CoreClasses/")
include_directories("../../OpenGLRendering/")
include_directories("../../Recast")
include_directories("../../Detour")
include_directories("../../DebugUtils")
include_directories("../../DetourTileCache")

add_executable(InteractionTests
    "main.cpp"
    "TestHarness.h"
    "PhysicsRegistrationTests.cpp"
    "CommandChannelTests.cpp"
    "RegionOwnershipTests.cpp"
    "NetworkIdSpaceTests.cpp"
    "ContactOrderingTests.cpp"
    "HaloShadowTests.cpp"
    "ContactSymmetryTests.cpp"
    "TaskPoolTests.cpp"
    "HandoffPacketTests.cpp"
    "PacketSizeTests.cpp"
    "HandoffCustodyTests.cpp"
    "InjectionScheduleTests.cpp"
    "NetworkCountersTests.cpp"
    "HaloBoundTests.cpp"
    "ObliqueWorkloadTests.cpp"
)

# The engine headers assume these are already included - the role targets get them
# via the same precompiled-header list, so without it PhysicsSystem.h fails on
# std::set and PhysicsObject.h on Matrix3.
target_precompile_headers(InteractionTests PRIVATE
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

    "../../NCLCoreClasses/Vector2i.h"
    "../../NCLCoreClasses/Vector3i.h"
    "../../NCLCoreClasses/Vector4i.h"

    "../../NCLCoreClasses/Vector2.h"
    "../../NCLCoreClasses/Vector3.h"
    "../../NCLCoreClasses/Vector4.h"
    "../../NCLCoreClasses/Quaternion.h"
    "../../NCLCoreClasses/Plane.h"
    "../../NCLCoreClasses/Matrix2.h"
    "../../NCLCoreClasses/Matrix3.h"
    "../../NCLCoreClasses/Matrix4.h"

    "../../NCLCoreClasses/GameTimer.h"
)

if(MSVC)
    target_compile_definitions(InteractionTests PRIVATE
        "UNICODE;"
        "_UNICODE"
        "WIN32_LEAN_AND_MEAN"
        "_WINSOCKAPI_"
        "_WINSOCK2API_"
        "_WINSOCK_DEPRECATED_NO_WARNINGS"
    )
    target_compile_options(InteractionTests PRIVATE
        /permissive-;
        /std:c++latest;
        /W3
    )
    target_link_libraries(InteractionTests LINK_PUBLIC "Winmm.lib")
endif()

target_link_libraries(InteractionTests LINK_PUBLIC NCLCoreClasses)
target_link_libraries(InteractionTests LINK_PUBLIC CSC8503CoreClasses)
target_link_libraries(InteractionTests LINK_PUBLIC OpenGLRendering)
target_link_libraries(InteractionTests LINK_PUBLIC Recast)
target_link_libraries(InteractionTests LINK_PUBLIC Detour)
target_link_libraries(InteractionTests LINK_PUBLIC DebugUtils)
target_link_libraries(InteractionTests LINK_PUBLIC DetourTileCache)

set_target_properties(InteractionTests PROPERTIES FOLDER "Tools")
