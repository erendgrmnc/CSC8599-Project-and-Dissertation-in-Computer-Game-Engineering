# PhysicsSystem Dynamic Registration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let `PhysicsSystem` accept and release objects after the first physics tick, so an object created at runtime is integrated, predicted and handed off like any pre-seeded one — and so removing an object does not leave dangling pointers in the collision sets.

**Architecture:** `BroadPhase` keeps its one-time bulk seed, but the sentinel becomes an explicit `mBroadphaseSeeded` flag instead of `mStaticTree.Empty()`. Two new public methods, `RegisterObject` / `UnregisterObject`, maintain membership incrementally after that seed. Unregistration is **deferred** — the integrators and broadphase iterate `mDynamicObjectList` by index, so erasing mid-iteration is undefined — and is drained at the top of `PhysicsSystem::Update`, where no iteration is in progress. The existing path is untouched: the world is fully seeded before the first tick, so the bulk seed still does all the work it does today.

**Tech Stack:** C++20, MSVC x64, CMake (Visual Studio 17 2022 generator). No test framework in the repo — this plan introduces a dependency-free assert harness as `tools/InteractionTests`.

**Spec:** `docs/superpowers/specs/2026-08-16-interactions-toolset-design.md` — §0.2 (the blocker), §3.1 (the concrete fix), §7.3 Tier 0 (the test harness).

## Global Constraints

- **C++20.** `std::erase`, `std::erase_if` and `std::is_trivially_copyable_v` are all available and expected.
- **Do not change existing physics behaviour.** The dissertation baseline (`ticks-server*.csv`) was captured against current behaviour. Any measurable change to physics time invalidates it. The bulk-seed path must remain byte-identical for a world that is fully populated before the first tick.
- **Add new files to the owning `CMake*.cmake`,** not just to disk. `CSC8503CoreClasses/CMakePC.cmake` lists that module's sources.
- **Namespace:** engine root is `NCL`; these classes are in `NCL::CSC8503`.
- **Member naming:** `mCamelCase` for members, `PascalCase` for methods, `SCREAMING_SNAKE` for file-scope constants. Match the surrounding file.
- **No new global compile definitions.** The test target must not perturb the four role builds.
- **Commit convention:** short lowercase `type(scope): summary`, no co-author trailers, one logical change per commit.

---

## File Structure

| File | Responsibility |
|---|---|
| `tools/InteractionTests/TestHarness.h` | *Create.* ~60-line assert/registry harness. No dependencies beyond the standard library. |
| `tools/InteractionTests/PhysicsRegistrationTests.cpp` | *Create.* All tests for this increment. |
| `tools/InteractionTests/main.cpp` | *Create.* Runs the registry, returns non-zero on failure. |
| `tools/InteractionTests/CMakeLists.txt` | *Create.* One console target linking the same libraries as `EntryPointServer`. |
| `CMakeLists.txt` | *Modify.* One `add_subdirectory` line. |
| `CSC8503CoreClasses/PhysicsSystem.h` | *Modify.* Public `RegisterObject`/`UnregisterObject`; protected `FlushPendingUnregisters`, `mBroadphaseSeeded`, `mPendingUnregister`. |
| `CSC8503CoreClasses/PhysicsSystem.cpp` | *Modify.* Seeded flag in `BroadPhase`; flush at top of `Update`; reset in `Clear`; the three new method bodies. |

Tests live outside `CSC8503CoreClasses` deliberately: the module is compiled into all four roles, and a test target inside it would be linked into shipped binaries.

---

### Task 1: Test harness that builds and runs

Nothing can be test-driven until there is a runner. This task delivers a target that compiles, links against the engine, and reports pass/fail — proven by one test asserting current, unmodified behaviour.

**Files:**
- Create: `tools/InteractionTests/TestHarness.h`
- Create: `tools/InteractionTests/main.cpp`
- Create: `tools/InteractionTests/PhysicsRegistrationTests.cpp`
- Create: `tools/InteractionTests/CMakeLists.txt`
- Modify: `CMakeLists.txt` (after the `add_subdirectory(DetourTileCache)` line, ~line 76)

**Interfaces:**
- Consumes: nothing.
- Produces: `TEST(name) { ... }` macro auto-registering a test; `CHECK(cond)`, `CHECK_EQ(a, b)`, `CHECK_NEAR(a, b, tol)`; `NCL::Testing::RunAllTests()` returning the failure count.

- [ ] **Step 1: Write the harness header**

`tools/InteractionTests/TestHarness.h`:

```cpp
#pragma once
// Dependency-free test harness. The repo has no test framework and adding one
// would mean a package manager the four role builds do not otherwise need.
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <cmath>

namespace NCL::Testing {

	struct TestCase {
		std::string name;
		std::function<void(int&)> body;
	};

	inline std::vector<TestCase>& Registry() {
		static std::vector<TestCase> registry;
		return registry;
	}

	struct AutoRegister {
		AutoRegister(const std::string& name, std::function<void(int&)> body) {
			Registry().push_back({ name, std::move(body) });
		}
	};

	// Returns the number of FAILED tests, so main can use it as an exit code.
	inline int RunAllTests() {
		int failed = 0;
		for (auto& test : Registry()) {
			int failuresInTest = 0;
			std::cout << "[ RUN      ] " << test.name << "\n";
			test.body(failuresInTest);
			if (failuresInTest == 0) {
				std::cout << "[       OK ] " << test.name << "\n";
			}
			else {
				std::cout << "[  FAILED  ] " << test.name << " (" << failuresInTest << " checks)\n";
				++failed;
			}
		}
		std::cout << "\n" << Registry().size() - failed << " passed, " << failed << " failed.\n";
		return failed;
	}
}

// The int& parameter is the per-test failure counter the CHECK macros increment.
#define TEST(NAME)                                                                    \
	static void NAME(int& claudeTestFailures);                                        \
	static NCL::Testing::AutoRegister claudeAutoRegister_##NAME(#NAME, NAME);         \
	static void NAME(int& claudeTestFailures)

#define CHECK(COND)                                                                   \
	do {                                                                              \
		if (!(COND)) {                                                                \
			++claudeTestFailures;                                                     \
			std::cout << "    FAIL " << __FILE__ << ":" << __LINE__                   \
				<< "  expected: " << #COND << "\n";                                   \
		}                                                                             \
	} while (false)

#define CHECK_EQ(A, B)                                                                \
	do {                                                                              \
		auto claudeLhs = (A);                                                         \
		auto claudeRhs = (B);                                                         \
		if (!(claudeLhs == claudeRhs)) {                                              \
			++claudeTestFailures;                                                     \
			std::cout << "    FAIL " << __FILE__ << ":" << __LINE__                   \
				<< "  " << #A << " == " << #B                                         \
				<< "  (got " << claudeLhs << " vs " << claudeRhs << ")\n";             \
		}                                                                             \
	} while (false)

#define CHECK_NEAR(A, B, TOL)                                                         \
	do {                                                                              \
		const double claudeDiff = std::fabs(double(A) - double(B));                   \
		if (!(claudeDiff <= double(TOL))) {                                           \
			++claudeTestFailures;                                                     \
			std::cout << "    FAIL " << __FILE__ << ":" << __LINE__                   \
				<< "  " << #A << " ~= " << #B                                         \
				<< "  (diff " << claudeDiff << " > " << double(TOL) << ")\n";          \
		}                                                                             \
	} while (false)
```

- [ ] **Step 2: Write the runner**

`tools/InteractionTests/main.cpp`:

```cpp
#include "TestHarness.h"

int main() {
	return NCL::Testing::RunAllTests();
}
```

- [ ] **Step 3: Write the first test — current behaviour, must pass unmodified**

This test asserts what the engine does *today*: a pre-seeded object integrates. It is the
harness's own smoke test. `Profiler::GetIntegratedObjects()` is the observation point —
`IntegrateAccel` already publishes its count there, so no production API has to change.

`tools/InteractionTests/PhysicsRegistrationTests.cpp`:

```cpp
#include "TestHarness.h"

#include "GameWorld.h"
#include "GameObject.h"
#include "PhysicsSystem.h"
#include "PhysicsObject.h"
#include "AABBVolume.h"
#include "Profiler.h"

using namespace NCL;
using namespace NCL::CSC8503;

namespace {
	// A dt large enough to guarantee at least one substep at the default 120 Hz,
	// so a single Update call always integrates.
	constexpr float TICK_DT = 0.05f;

	// Builds a dynamic, physics-enabled, network-active cube. Network-active matters:
	// PredictFuturePositions skips objects that are not.
	GameObject* MakeDynamicCube(const Vector3& position) {
		GameObject* cube = new GameObject(NoSpecialFeatures, "cube");
		cube->SetBoundingVolume(new AABBVolume(Vector3(1, 1, 1)));
		cube->GetTransform().SetScale(Vector3(2, 2, 2)).SetPosition(position);
		cube->SetPhysicsObject(new PhysicsObject(&cube->GetTransform(), cube->GetBoundingVolume()));
		cube->GetPhysicsObject()->SetInverseMass(1.0f);
		cube->GetPhysicsObject()->InitCubeInertia();
		cube->SetActive(true);
		return cube;
	}

	// A static floor, so the quadtree is non-empty exactly as it is in a real server
	// (ServerWorldManager's constructor adds one before the first tick).
	GameObject* MakeFloor() {
		GameObject* floor = new GameObject(StaticObj, "floor");
		floor->SetBoundingVolume(new AABBVolume(Vector3(100, 1, 100)));
		floor->GetTransform().SetScale(Vector3(200, 2, 200)).SetPosition(Vector3(0, -10, 0));
		floor->SetPhysicsObject(new PhysicsObject(&floor->GetTransform(), floor->GetBoundingVolume()));
		floor->GetPhysicsObject()->SetInverseMass(0.0f);
		floor->GetPhysicsObject()->InitCubeInertia();
		floor->SetActive(true);
		return floor;
	}
}

TEST(PreSeededObjectIsIntegrated) {
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	world.AddGameObject(MakeFloor());
	GameObject* cube = MakeDynamicCube(Vector3(0, 50, 0));
	world.AddGameObject(cube);

	physics.Update(TICK_DT);

	// One dynamic object in the world, so exactly one integrated.
	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);
	// Gravity is -9.8 on Y, so a falling cube has negative Y velocity.
	CHECK(cube->GetPhysicsObject()->GetLinearVelocity().y < 0.0f);

	world.ClearAndErase();
}
```

- [ ] **Step 4: Write the CMake target**

`tools/InteractionTests/CMakeLists.txt`. The link set mirrors `EntryPoint/CMakeDistributedRoles.cmake`
minus the role library — the engine pulls in the OpenGL backend at link time even though no window
is created.

```cmake
# Tier 0 unit tests (see the interactions design spec, section 7.3).
#
# Deliberately a separate executable rather than anything linked into a role: it
# must never be able to perturb the four shipped binaries. It links the same
# libraries EntryPointServer does, minus DistributedGameServer, because the
# engine's static libs are interdependent and pull in the GL backend at link time.
project(InteractionTests CXX)

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
```

- [ ] **Step 5: Register the subdirectory**

In the root `CMakeLists.txt`, immediately after the `add_subdirectory(DetourTileCache)` line:

```cmake
add_subdirectory(DetourTileCache)

# Tier 0 unit tests. Built in both configures; it links libraries only, never a
# role entry point, so it cannot change what the four shipped binaries contain.
add_subdirectory(tools/InteractionTests)
```

- [ ] **Step 6: Configure and build**

```powershell
cmake -G "Visual Studio 17 2022" -A x64 .
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
```

Expected: builds clean. If `AABBVolume.h` or `Profiler.h` are not found, check the
`include_directories` paths above resolve from `tools/InteractionTests/`.

- [ ] **Step 7: Run — must pass**

```powershell
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: `[       OK ] PreSeededObjectIsIntegrated` and `1 passed, 0 failed.`, exit code 0.

If `Profiler::GetIntegratedObjects()` returns 0, the substep did not run — raise `TICK_DT`.
If it returns 2, the floor is being counted as dynamic — confirm `StaticObj` is in
`PhysicsSystem`'s `STATIC_COLLISION_LAYERS`.

- [ ] **Step 8: Commit**

```bash
git add tools/InteractionTests CMakeLists.txt
git commit -m "test: add tier 0 unit test harness"
```

---

### Task 2: Replace the seed sentinel with an explicit flag

`BroadPhase` decides "have I seeded yet?" by asking whether the static quadtree is empty. That is
wrong in its own right — a world with no static objects re-seeds every single tick, appending every
dynamic object to `mDynamicObjectList` again — and it cannot express "seeded, and now accepting
incremental changes". This task swaps the sentinel with no behaviour change for a world that has a
floor, which every server world does.

**Files:**
- Modify: `CSC8503CoreClasses/PhysicsSystem.h` (protected members, ~line 108)
- Modify: `CSC8503CoreClasses/PhysicsSystem.cpp` (`BroadPhase` ~line 453, `Clear` ~line 61)
- Test: `tools/InteractionTests/PhysicsRegistrationTests.cpp`

**Interfaces:**
- Consumes: the harness from Task 1.
- Produces: `PhysicsSystem::mBroadphaseSeeded` (protected `bool`, default `false`), reset by `Clear()`.

- [ ] **Step 1: Write the failing test**

Append to `PhysicsRegistrationTests.cpp`:

```cpp
// A world with no static geometry seeds mDynamicObjectList once, not once per tick.
// Under the old mStaticTree.Empty() sentinel the tree stays empty forever, so the
// seed loop re-runs every tick and the same cube is pushed again and again.
TEST(SeedRunsOnceEvenWithNoStaticObjects) {
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	world.AddGameObject(MakeDynamicCube(Vector3(0, 50, 0)));

	physics.Update(TICK_DT);
	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);

	physics.Update(TICK_DT);
	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);

	physics.Update(TICK_DT);
	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);

	world.ClearAndErase();
}
```

- [ ] **Step 2: Run it — must fail**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: `SeedRunsOnceEvenWithNoStaticObjects` FAILS — the second check reports 2, the third 3.

- [ ] **Step 3: Add the flag to the header**

In `PhysicsSystem.h`, in the protected member block, directly after the
`std::vector<GameObject*> mDynamicObjectList;` line:

```cpp
			std::vector<GameObject*> mDynamicObjectList;

			// Replaces the old mStaticTree.Empty() sentinel for "has the one-time bulk
			// seed run?". The tree is the wrong thing to ask: a world with no static
			// geometry leaves it empty forever, so the seed re-ran every tick and
			// duplicated every dynamic object. It also cannot express "seeded, now
			// accepting incremental Register/Unregister".
			bool mBroadphaseSeeded = false;
```

- [ ] **Step 4: Use the flag in `BroadPhase`**

In `PhysicsSystem.cpp`, replace the line `if(mStaticTree.Empty()) {` (~453) and add the
assignment at the end of that block. The loop body itself is unchanged:

```cpp
	if (!mBroadphaseSeeded) {
		for (auto i = first; i != last; i++) {
			Vector3 halfSizes;
			if (!(*i)->GetBroadphaseAABB(halfSizes)) continue;
			if ((*i)->GetCollisionLayer() & STATIC_COLLISION_LAYERS) {
				Vector3 pos = (*i)->GetTransform().GetPosition() + (*i)->GetBoundingVolume()->GetOffset();
				mStaticTree.Insert(*i, pos, halfSizes, true);
			}
			else {
				mDynamicObjectList.push_back(*i);
			}
		}
		mBroadphaseSeeded = true;
	}
```

Note the pre-existing `if (first == last) return;` above stays where it is, so an empty world
returns before the flag is set and still seeds properly once objects arrive.

- [ ] **Step 5: Reset the flag in `Clear`**

`Clear()` empties `mDynamicObjectList`, so it must also allow a re-seed — otherwise a cleared
world is permanently empty to the physics system. In `PhysicsSystem.cpp` (~line 61):

```cpp
void PhysicsSystem::Clear() {
	mAllCollisions.clear();
	mDynamicObjectList.clear();
	// Without this, a cleared world can never be re-seeded and nothing would ever
	// integrate again.
	mBroadphaseSeeded = false;
}
```

- [ ] **Step 6: Run the tests — all must pass**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: `2 passed, 0 failed.`

- [ ] **Step 7: Commit**

```bash
git add CSC8503CoreClasses/PhysicsSystem.h CSC8503CoreClasses/PhysicsSystem.cpp tools/InteractionTests/PhysicsRegistrationTests.cpp
git commit -m "fix(physics): seed broadphase once via explicit flag"
```

---

### Task 3: `RegisterObject` — accept objects after the seed

**Files:**
- Modify: `CSC8503CoreClasses/PhysicsSystem.h` (public section, after `SetPredictionHorizon`/`GetPredictionHorizon`, ~line 49)
- Modify: `CSC8503CoreClasses/PhysicsSystem.cpp` (new method, place directly above `BroadPhase`)
- Test: `tools/InteractionTests/PhysicsRegistrationTests.cpp`

**Interfaces:**
- Consumes: `mBroadphaseSeeded` from Task 2.
- Produces: `void PhysicsSystem::RegisterObject(GameObject* o)` — public, null-safe, idempotent.

- [ ] **Step 1: Write the failing tests**

Append to `PhysicsRegistrationTests.cpp`:

```cpp
// The blocker this whole increment exists to remove: an object added after the
// first tick is invisible to IntegrateAccel and never falls.
TEST(RuntimeObjectIsIntegratedAfterRegister) {
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	world.AddGameObject(MakeFloor());
	physics.Update(TICK_DT);
	CHECK_EQ(Profiler::GetIntegratedObjects(), 0);

	GameObject* cube = MakeDynamicCube(Vector3(0, 50, 0));
	world.AddGameObject(cube);
	physics.RegisterObject(cube);

	physics.Update(TICK_DT);

	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);
	CHECK(cube->GetPhysicsObject()->GetLinearVelocity().y < 0.0f);

	world.ClearAndErase();
}

// The under-appreciated half of the blocker: PredictFuturePositions also walks
// mDynamicObjectList, so an unregistered object gets no predicted position and
// would never trigger a handoff.
TEST(RuntimeObjectGetsPredictedPosition) {
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	world.AddGameObject(MakeFloor());
	physics.Update(TICK_DT);

	GameObject* cube = MakeDynamicCube(Vector3(0, 50, 0));
	cube->GetPhysicsObject()->SetLinearVelocity(Vector3(10, 0, 0));
	world.AddGameObject(cube);
	physics.RegisterObject(cube);

	physics.PredictFuturePositions(TICK_DT);

	// Moving +X at 10 u/s over the default 0.1 s horizon lands about 1 unit ahead.
	CHECK(cube->GetTransform().GetPredictedPosition().x > 0.5f);

	world.ClearAndErase();
}

// Registering the same object twice must not integrate it twice - a double entry
// would apply gravity twice per tick and silently corrupt every measurement.
TEST(RegisterIsIdempotent) {
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	world.AddGameObject(MakeFloor());
	physics.Update(TICK_DT);

	GameObject* cube = MakeDynamicCube(Vector3(0, 50, 0));
	world.AddGameObject(cube);
	physics.RegisterObject(cube);
	physics.RegisterObject(cube);

	physics.Update(TICK_DT);

	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);

	world.ClearAndErase();
}

// A null pointer must be ignored, not dereferenced.
TEST(RegisterNullIsSafe) {
	GameWorld world;
	PhysicsSystem physics(world);

	world.AddGameObject(MakeFloor());
	physics.Update(TICK_DT);
	physics.RegisterObject(nullptr);
	physics.Update(TICK_DT);

	CHECK_EQ(Profiler::GetIntegratedObjects(), 0);

	world.ClearAndErase();
}
```

- [ ] **Step 2: Run — must fail to compile**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
```

Expected: `error C2039: 'RegisterObject': is not a member of 'NCL::CSC8503::PhysicsSystem'`.

- [ ] **Step 3: Declare it in the header**

In `PhysicsSystem.h`, at the end of the public section (after `GetPredictionHorizon`, before
`protected:`):

```cpp
			// Incremental broadphase membership, for objects created or removed after
			// the first physics tick. The initial world is still bulk-seeded by the
			// first BroadPhase call exactly as before, so this changes nothing for the
			// existing path. GameWorld::AddGameObject is not virtual and PhysicsSystem
			// does not observe the world, so callers must invoke these explicitly.
			void RegisterObject(GameObject* o);
			void UnregisterObject(GameObject* o);
```

- [ ] **Step 4: Implement `RegisterObject`**

In `PhysicsSystem.cpp`, directly above `void PhysicsSystem::BroadPhase()`:

```cpp
void PhysicsSystem::RegisterObject(GameObject* o) {
	if (o == nullptr) {
		return;
	}

	// Before the bulk seed, membership is BroadPhase's job. Registering now would
	// put the object in the list and the seed would then add it a second time.
	if (!mBroadphaseSeeded) {
		return;
	}

	Vector3 halfSizes;
	if (!o->GetBroadphaseAABB(halfSizes)) {
		return;
	}

	if (o->GetCollisionLayer() & STATIC_COLLISION_LAYERS) {
		const Vector3 pos = o->GetTransform().GetPosition() + o->GetBoundingVolume()->GetOffset();
		mStaticTree.Insert(o, pos, halfSizes, true);
		return;
	}

	// Idempotent: a double entry integrates the object twice per tick, which reads
	// as doubled gravity and corrupts every derived measurement.
	if (std::find(mDynamicObjectList.begin(), mDynamicObjectList.end(), o) != mDynamicObjectList.end()) {
		return;
	}

	mDynamicObjectList.push_back(o);
}
```

Add `#include <algorithm>` to the include block at the top of `PhysicsSystem.cpp` if `std::find`
does not resolve.

- [ ] **Step 5: Run the tests — all must pass**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: `6 passed, 0 failed.`

If `RuntimeObjectGetsPredictedPosition` fails, check the cube is network-active — `SetActive(true)`
in `MakeDynamicCube` sets `mIsNetworkActive`, and `PredictFuturePositions` skips objects where it
is false.

- [ ] **Step 6: Commit**

```bash
git add CSC8503CoreClasses/PhysicsSystem.h CSC8503CoreClasses/PhysicsSystem.cpp tools/InteractionTests/PhysicsRegistrationTests.cpp
git commit -m "feat(physics): register objects after broadphase seed"
```

---

### Task 4: `UnregisterObject` — deferred removal with collision purge

The dangerous half. `IntegrateAccel`, `IntegrateVelocity` and `BroadPhase` all iterate
`mDynamicObjectList` by index, so removal must never happen mid-iteration. And `mAllCollisions`
holds raw `GameObject*` that `UpdateCollisionList` dereferences for up to `mNumCollisionFrames`
frames after contact ends — leaving an entry behind is a use-after-free.

**Files:**
- Modify: `CSC8503CoreClasses/PhysicsSystem.h` (protected members and method)
- Modify: `CSC8503CoreClasses/PhysicsSystem.cpp` (`Update` ~line 77, `Clear` ~line 61, new method)
- Test: `tools/InteractionTests/PhysicsRegistrationTests.cpp`

**Interfaces:**
- Consumes: `RegisterObject` from Task 3; `mBroadphaseSeeded` from Task 2.
- Produces: `void PhysicsSystem::UnregisterObject(GameObject* o)` — public, null-safe, deferred;
  `void PhysicsSystem::FlushPendingUnregisters()` — protected, drained at the top of `Update`;
  `std::vector<GameObject*> mPendingUnregister` — protected.

- [ ] **Step 1: Write the failing tests**

Append to `PhysicsRegistrationTests.cpp`. The collision test needs a probe subclass, so add it to
the anonymous namespace at the top of the file first:

```cpp
	// Counts OnCollisionEnd calls. Unregistering an object must NOT fire it: the
	// object is being removed, not separating from a contact.
	class ProbeObject : public GameObject {
	public:
		ProbeObject() : GameObject(NoSpecialFeatures, "probe") {}
		int collisionEndCount = 0;
		void OnCollisionEnd(GameObject* otherObject) override {
			++collisionEndCount;
		}
	};
```

Then the tests:

```cpp
TEST(UnregisteredObjectStopsIntegrating) {
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	world.AddGameObject(MakeFloor());
	GameObject* cube = MakeDynamicCube(Vector3(0, 50, 0));
	world.AddGameObject(cube);

	physics.Update(TICK_DT);
	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);

	physics.UnregisterObject(cube);
	physics.Update(TICK_DT);

	CHECK_EQ(Profiler::GetIntegratedObjects(), 0);

	world.ClearAndErase();
}

// Removal must be deferred to the top of the next Update: the integrators walk
// mDynamicObjectList by index, so erasing during a tick is undefined behaviour.
// Observable consequence - the object still integrates on the tick during which
// it was unregistered, never mid-tick.
TEST(UnregisterIsDeferredNotImmediate) {
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	world.AddGameObject(MakeFloor());
	GameObject* cube = MakeDynamicCube(Vector3(0, 50, 0));
	world.AddGameObject(cube);
	physics.Update(TICK_DT);

	physics.UnregisterObject(cube);
	// Not yet flushed, so the list is untouched until the next Update begins.
	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);

	physics.Update(TICK_DT);
	CHECK_EQ(Profiler::GetIntegratedObjects(), 0);

	world.ClearAndErase();
}

// The use-after-free guard. Two overlapping cubes generate a collision record;
// after unregistering one, no further callback may reference it.
TEST(UnregisterPurgesCollisionRecords) {
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(false);

	world.AddGameObject(MakeFloor());

	ProbeObject* probe = new ProbeObject();
	probe->SetBoundingVolume(new AABBVolume(Vector3(1, 1, 1)));
	probe->GetTransform().SetScale(Vector3(2, 2, 2)).SetPosition(Vector3(0, 20, 0));
	probe->SetPhysicsObject(new PhysicsObject(&probe->GetTransform(), probe->GetBoundingVolume()));
	probe->GetPhysicsObject()->SetInverseMass(1.0f);
	probe->GetPhysicsObject()->InitCubeInertia();
	probe->SetActive(true);
	world.AddGameObject(probe);

	// Overlapping the probe, so a contact is generated immediately.
	GameObject* other = MakeDynamicCube(Vector3(0, 20, 0));
	world.AddGameObject(other);

	physics.Update(TICK_DT);
	const int endsBeforeRemoval = probe->collisionEndCount;

	physics.UnregisterObject(probe);

	// mNumCollisionFrames is 5, so run well past the window a stale record would
	// survive. Any purge failure shows up as an extra OnCollisionEnd here, and in a
	// real run as a dereference of freed memory.
	for (int tick = 0; tick < 10; ++tick) {
		physics.Update(TICK_DT);
	}

	CHECK_EQ(probe->collisionEndCount, endsBeforeRemoval);

	world.ClearAndErase();
}

TEST(UnregisterNullIsSafe) {
	GameWorld world;
	PhysicsSystem physics(world);

	world.AddGameObject(MakeFloor());
	physics.Update(TICK_DT);
	physics.UnregisterObject(nullptr);
	physics.Update(TICK_DT);

	CHECK_EQ(Profiler::GetIntegratedObjects(), 0);

	world.ClearAndErase();
}
```

- [ ] **Step 2: Run — must fail to compile**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
```

Expected: `error C2039: 'UnregisterObject': is not a member of 'NCL::CSC8503::PhysicsSystem'`.
(`UnregisterObject` was declared alongside `RegisterObject` in Task 3 step 3 — if it was, this
fails at link time with `LNK2019` instead. Either failure is the expected red.)

- [ ] **Step 3: Add the protected members and method to the header**

In `PhysicsSystem.h`, in the protected section next to the other helpers (near `ClearForces`):

```cpp
			// Drains mPendingUnregister. Called at the top of Update, where no
			// iteration over mDynamicObjectList is in progress.
			void FlushPendingUnregisters();
```

And with the protected data, after `mBroadphaseSeeded`:

```cpp
			// Removal is deferred, never immediate: IntegrateAccel, IntegrateVelocity
			// and BroadPhase all walk mDynamicObjectList by index, so erasing from it
			// during a tick is undefined behaviour.
			std::vector<GameObject*> mPendingUnregister;
```

- [ ] **Step 4: Implement `UnregisterObject` and `FlushPendingUnregisters`**

In `PhysicsSystem.cpp`, directly below `RegisterObject`:

```cpp
void PhysicsSystem::UnregisterObject(GameObject* o) {
	if (o == nullptr) {
		return;
	}

	// QuadTree exposes no removal operation, so a static object cannot be taken out
	// of mStaticTree. Runtime spawn and destroy only ever produce dynamic objects;
	// this guard exists so a future caller gets a diagnostic instead of silently
	// leaving a dangling pointer in the tree.
	if (o->GetCollisionLayer() & STATIC_COLLISION_LAYERS) {
		std::cout << "WARNING: UnregisterObject called on static object '" << o->GetName()
			<< "' - the quadtree has no removal operation, so it will remain in the broadphase.\n";
		return;
	}

	mPendingUnregister.push_back(o);
}

void PhysicsSystem::FlushPendingUnregisters() {
	if (mPendingUnregister.empty()) {
		return;
	}

	for (GameObject* o : mPendingUnregister) {
		std::erase(mDynamicObjectList, o);

		// UpdateCollisionList dereferences CollisionInfo::a and ::b to fire
		// OnCollisionEnd for up to mNumCollisionFrames after a contact ends, so a
		// leftover record referencing a destroyed object is a use-after-free.
		// Deliberately no OnCollisionEnd here: the object is being removed from the
		// simulation, not separating from a contact.
		const auto referencesObject = [o](const CollisionDetection::CollisionInfo& info) {
			return info.a == o || info.b == o;
		};
		std::erase_if(mAllCollisions, referencesObject);
		std::erase_if(mBroadphaseCollisions, referencesObject);
		std::erase_if(mBroadphaseCollisionsVec, referencesObject);
	}

	mPendingUnregister.clear();
}
```

`PhysicsSystem.cpp` already includes `<functional>`; add `#include <iostream>` if `std::cout` does
not resolve.

- [ ] **Step 5: Drain at the top of `Update`**

In `PhysicsSystem.cpp`, `Update` (~line 77) — the flush must be the first statement, before
`mDTOffset` accumulates and before any iteration begins:

```cpp
void PhysicsSystem::Update(float dt) {
	// Must run before anything iterates mDynamicObjectList this tick.
	FlushPendingUnregisters();

	mDTOffset += dt; //We accumulate time delta here - there might be remainders from previous frame!
```

- [ ] **Step 6: Clear the pending list in `Clear`**

```cpp
void PhysicsSystem::Clear() {
	mAllCollisions.clear();
	mDynamicObjectList.clear();
	// Without this, a cleared world can never be re-seeded and nothing would ever
	// integrate again.
	mBroadphaseSeeded = false;
	// Anything queued refers to objects the caller is about to destroy.
	mPendingUnregister.clear();
}
```

- [ ] **Step 7: Run the tests — all must pass**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:InteractionTests /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: `10 passed, 0 failed.`

- [ ] **Step 8: Commit**

```bash
git add CSC8503CoreClasses/PhysicsSystem.h CSC8503CoreClasses/PhysicsSystem.cpp tools/InteractionTests/PhysicsRegistrationTests.cpp
git commit -m "feat(physics): deferred unregister with collision purge"
```

---

### Task 5: Prove the baseline is unchanged

The whole increment is only safe if the existing simulation behaves identically. This task verifies
that empirically against the metrics substrate, and is the gate on the increment.

**Files:**
- No source changes expected. If this task finds a difference, the increment is wrong.

**Interfaces:**
- Consumes: everything from Tasks 2–4.
- Produces: a recorded before/after comparison for the dissertation's methods section.

- [ ] **Step 0: Promote the measurement script into the repo**

`measure.ps1` currently exists only in the session scratchpad, so the baseline it produced is not
reproducible by anyone else. Copy it to `tools/measure.ps1` and commit it before using it as a
gate. Change its hard-coded `$scratch` run directory to a `-OutDir` parameter defaulting to
`runs/` at the repo root, and add `runs/` to `.gitignore`.

```bash
git add tools/measure.ps1 .gitignore
git commit -m "tools: add bounded measurement run script"
```

- [ ] **Step 1: Build and deploy all roles in Release**

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
```

Expected: `deploy/Manager|Midware|Client|DistributedPhysicsServer/EntryPoint.exe` all present.

- [ ] **Step 2: Re-run the fixed-seed measurement scenario**

```powershell
powershell -ExecutionPolicy Bypass -File tools\measure.ps1 -Servers 2 -Objects 400 -Seconds 60 -Tag s2-o400-postreg
```

Expected: two CSVs written, `Headless run complete after 60.0…s`, zero dropped samples.

- [ ] **Step 3: Compare against the pre-change baseline**

The reference run is `s2-o400` (2 servers, 400 objects, seed 42, `--fixed-step`, Release).
Compare, per server:

| Quantity | Baseline (server 0 / server 1) | Requirement |
|---|---|---|
| Final `hoSent` / `hoRecv` | 44 / 1 and 1 / 44 | **Exactly equal.** Determinism is per-configuration, so at the same seed and config these must match bit for bit. |
| Final object counts | 357 and 43, summing to 400 | **Exactly equal.** |
| `hoFail` | 0 | **Must stay 0.** |
| Tick-cost p50 | 0.0170 ms / 0.0094 ms | Within noise (~±10%). A systematic increase means the flush or the `std::find` is on a hot path. |

A handoff-count difference is a **hard stop** — it means physics membership changed, and the
increment must be re-examined before going further.

- [ ] **Step 4: Record the comparison in the spec**

Append an "implementation notes" entry to
`docs/superpowers/specs/2026-08-16-interactions-toolset-design.md` recording: both runs' handoff
counts and object counts, the p50/p99 tick costs, and the fact that increment 1 shipped without
any call site yet — `RegisterObject` and `UnregisterObject` exist and are tested but nothing in
`ServerWorldManager` calls them until increment 5 (spawn) and increment 6 (destroy).

- [ ] **Step 5: Update the audit notes in CLAUDE.md**

The "Verified-state warnings" block says the integrator and broadphase cannot accept runtime
objects. Replace that claim with the current state: the physics system now supports incremental
membership, but no distributed code calls it yet.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/2026-08-16-interactions-toolset-design.md CLAUDE.md
git commit -m "docs: record increment 1 baseline comparison"
```

---

## Already done — do not redo

Spec §3.1 lists three details as the substance of this increment. The third is already shipped:

- **`ClearForces` null guard** — landed during the Track 0 correctness work
  (`PhysicsSystem.cpp:631`). It already skips objects with no `PhysicsObject`, with a comment
  saying why. Nothing to do.

Spec §0.3's checked `find()` replacing `mCreatedObjectPool.at()` in `StartHandlingObject` and
`HandleOutgoingObject` is likewise already done, as are the I5 handoff-parity counters
(`hoSent`/`hoRecv`/`hoFail` in `@@STAT`) that Task 5 uses as its gate.

## Out of scope for this plan

- **No call sites.** `ServerWorldManager` does not call `RegisterObject`/`UnregisterObject` yet;
  there is nothing to spawn or destroy until increments 5 and 6. Wiring them early would change
  handoff behaviour with no feature to justify it.
- **Static object removal.** `QuadTree` has no removal operation. `UnregisterObject` warns and
  refuses for static objects. Adding quadtree removal is a separate piece of work and is not
  needed by any planned increment.
- **The command channel** (increment 3) is a separate plan — it shares no code with this one.
