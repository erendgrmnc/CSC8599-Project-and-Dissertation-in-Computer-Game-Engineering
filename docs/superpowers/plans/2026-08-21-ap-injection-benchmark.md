# AP-Comparable Injection Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reproduce Aura Projection's published benchmark — 160 objects/second injected for 60 seconds — so the evaluation has a bounded comparison against the direct ancestor instead of only comparing the system to itself.

**Architecture:** Three additive pieces plus analysis. A new `Cuboid` archetype (OBB) leaves the existing `Cube` untouched so E1–E8 stay valid. A new `--workload injection` starts from an empty world and spawns on a schedule that is a pure function of `(seed, index)`, so every server can walk the same schedule and spawn only its own share with no coordination. A new `substeps` metric column makes "frame" well-defined, and `analyse.py` gains AP's max-frame-time-per-5s-window report.

**Tech Stack:** C++20, MSVC x64, CMake-generated VS solution. ENet networking, strict-POD packets. Tests via the repo's dependency-free `TEST`/`CHECK` harness in `tools/InteractionTests`. Harness is PowerShell (`tools/measure.ps1`, `tools/run-experiments.ps1`) plus `tools/analyse.py` (stdlib only).

**Spec:** `docs/superpowers/specs/2026-08-21-ap-injection-benchmark-design.md`

## Global Constraints

- **Build Release, x64.** MSBuild at `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe`. Roles: `/t:EntryPointManager;EntryPointMidware;EntryPointServer` (backtick-escape the `;` in PowerShell). Tests: `/t:Tools\InteractionTests` — note the solution-folder prefix.
- **`ObjectArchetype` is append-only on the wire.** `Cube = 0` and `Sphere = 1` must keep their values and their behaviour. `Cube` uses `AABBVolume` and `InitSphereInertia(false)`; every measurement in E1–E8 used it. Do not "fix" it.
- **A new game-server flag must be added in FOUR places or it is silently ignored:** parsed in `DistributedGameServer/ServerStarter.cpp`, forwarded in `PhysicsServerMidware/ProgramStart.cpp`, and exposed in BOTH `tools/measure.ps1` and `tools/run-experiments.ps1`.
- **`-Values` in `run-experiments.ps1` is a comma-separated STRING**, not an array. `powershell -File` cannot parse array arguments.
- **`tools/analyse.py` is stdlib-only.** No third-party imports. `tools/test_analyse.py` must keep passing.
- **PowerShell scripts run under Windows PowerShell 5.1**: no `&&`, `||`, ternary, or null-coalescing — those are parser errors. Use `if/else`.
- **Pure, shared logic goes in `CSC8503CoreClasses/DistributedSystemCommonFiles/`** — precedent `RegionOwnership.h`, `NetworkIdSpace.h`, `HandoffCustody.h`. No `USEGL`/`DISTRIBUTEDSYSTEMACTIVE` guards; servers, client and the test target all include them.
- **New source files must be registered in the relevant CMake file**, not just created on disk.
- **Commit style:** short, understandable messages; **no `Co-Authored-By` trailer and no `Claude-Session` line**.
- **Current test baseline:** `InteractionTests` 124 passed / 0 failed; `test_analyse.py` 19 passed.

## File Structure

| File | Responsibility |
|---|---|
| `CSC8503CoreClasses/OBBVolume.h` | Fix the constructor's offset bug (prerequisite for a usable OBB). |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h` | Append `Cuboid = 2` to `ObjectArchetype`. |
| `DistributedGameServer/ServerWorldManager.h/.cpp` | `AddCuboidToWorld`; archetype dispatch; injection tick hook; injection velocity. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/InjectionSchedule.h` | **New.** The pure spawn schedule: `(seed, index) -> InjectionDraw`. Header-only. |
| `tools/InteractionTests/InjectionScheduleTests.cpp` | **New.** Unit tests for the schedule and the OBB fix. |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/MetricSink.h` | Add the `substeps` column to `TickSample` and the CSV header/row. |
| `tools/analyse.py` | AP frame-time report: max across servers per 5 s bucket, mean over repeats. |
| `docs/superpowers/results/2026-08-21-AP-injection.md` | **New.** Phase 1 results and the declared-deviation table. |

---

### Task 1: Fix the OBBVolume offset bug

A prerequisite, and free: no game object currently uses `OBBVolume`, so this cannot change any existing measurement.

**Files:**
- Modify: `CSC8503CoreClasses/OBBVolume.h:8-13`
- Create: `tools/InteractionTests/InjectionScheduleTests.cpp`
- Modify: `tools/InteractionTests/CMakeLists.txt` (the `add_executable` source list)

**Interfaces:**
- Consumes: nothing.
- Produces: `OBBVolume(halfDims, offset)` with a correctly-stored offset, for Task 2.

- [ ] **Step 1: Write the failing test**

Create `tools/InteractionTests/InjectionScheduleTests.cpp`:

```cpp
// The OBB volume's offset, and (from Task 3) the AP injection schedule.
//
// OBBVolume's constructor assigned `this->offset = halfDims` instead of the
// `offset` parameter, so the default (0,0,0) was ignored and every OBB's
// collision volume sat displaced by its own half-extents.
// CollisionDetection adds GetOffset() to the world position in every OBB path,
// so this is a real displacement, not a cosmetic field.
//
// Nothing constructed an OBBVolume before the Cuboid archetype, so fixing it
// cannot move any previously measured result.

#include "TestHarness.h"

#include "OBBVolume.h"
#include "AABBVolume.h"

using namespace NCL;
using namespace NCL::Maths;

TEST(OBBVolumeDefaultOffsetIsZero) {
	OBBVolume volume(Vector3(0.15f, 0.15f, 0.5f));
	const Vector3 offset = volume.GetOffset();
	CHECK(offset.x == 0.0f);
	CHECK(offset.y == 0.0f);
	CHECK(offset.z == 0.0f);
}

TEST(OBBVolumeKeepsAnExplicitOffset) {
	OBBVolume volume(Vector3(1.0f, 2.0f, 3.0f), Vector3(4.0f, 5.0f, 6.0f));
	const Vector3 offset = volume.GetOffset();
	CHECK(offset.x == 4.0f);
	CHECK(offset.y == 5.0f);
	CHECK(offset.z == 6.0f);
}

TEST(OBBVolumeMatchesAABBVolumeOffsetConvention) {
	// The two volume types must agree, or an object's collision shape would move
	// when its volume type changed.
	OBBVolume obb(Vector3(0.15f, 0.15f, 0.5f));
	AABBVolume aabb(Vector3(0.15f, 0.15f, 0.5f));
	CHECK(obb.GetOffset().x == aabb.GetOffset().x);
	CHECK(obb.GetOffset().y == aabb.GetOffset().y);
	CHECK(obb.GetOffset().z == aabb.GetOffset().z);
}

TEST(OBBVolumeReportsItsHalfDimensions) {
	OBBVolume volume(Vector3(0.15f, 0.15f, 0.5f));
	const Vector3 half = volume.GetHalfDimensions();
	CHECK(half.x == 0.15f);
	CHECK(half.y == 0.15f);
	CHECK(half.z == 0.5f);
}
```

Register it in `tools/InteractionTests/CMakeLists.txt` by adding `"InjectionScheduleTests.cpp"` to the `add_executable(InteractionTests ...)` source list, after the last existing entry.

- [ ] **Step 2: Run to verify it fails**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
```

Expected: `OBBVolumeDefaultOffsetIsZero` and `OBBVolumeMatchesAABBVolumeOffsetConvention` FAIL (offset reads back as the half-extents). If the new file is not compiled, regenerate with `cmake -G "Visual Studio 17 2022" -A x64 .` from the repo root and rebuild — do **not** delete `CMakeCache.txt`.

- [ ] **Step 3: Fix the constructor**

In `CSC8503CoreClasses/OBBVolume.h`, change the offset assignment so it stores the parameter:

```cpp
		OBBVolume(const Maths::Vector3& halfDims, const Maths::Vector3& offset = Maths::Vector3(0, 0, 0)) {
			type		= VolumeType::OBB;
			halfSizes	= halfDims;
			// Was `this->offset = halfDims`, which ignored the parameter and displaced
			// every OBB by its own half-extents. CollisionDetection adds GetOffset()
			// to the world position on every OBB path, so that was a real shift.
			this->offset = offset;
			this->applyPhysics = true;
		}
```

- [ ] **Step 4: Run to verify it passes**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
```

Expected: `128 passed, 0 failed.` (124 existing + 4 new), exit code 0.

- [ ] **Step 5: Commit**

```bash
git add CSC8503CoreClasses/OBBVolume.h tools/InteractionTests/InjectionScheduleTests.cpp tools/InteractionTests/CMakeLists.txt
git commit -m "fix(physics): OBBVolume ignored its offset parameter"
```

---

### Task 2: The Cuboid archetype

**Files:**
- Modify: `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h:47-50`
- Modify: `DistributedGameServer/ServerWorldManager.h` (declare `AddCuboidToWorld` beside `AddCubeToWorld` at `:53`)
- Modify: `DistributedGameServer/ServerWorldManager.cpp` (`CreateObjectFromArchetype`; new `AddCuboidToWorld`)

**Interfaces:**
- Consumes: fixed `OBBVolume(halfDims, offset)` (Task 1).
- Produces: `ObjectArchetype::Cuboid = 2`; `CSC8503::GameObject* AddCuboidToWorld(const CSC8503::Transform& transform, int count, int playerID) const`.

> **This task adds no new unit test, deliberately.** `AddCuboidToWorld` needs a live
> `GameWorld`, `PhysicsSystem` and border configuration, so it is not reachable from the
> dependency-free test harness. The properties that *are* pure — the OBB's dimensions and offset —
> are covered by Task 1. This task's verification is that all three roles link and the existing
> suite still passes; the archetype is genuinely exercised by Task 4's smoke test, which spawns
> cuboids for real. Do not invent a test that asserts nothing in order to have one.

- [ ] **Step 1: Append the archetype**

In `CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h`:

```cpp
	enum class ObjectArchetype : int {
		Cube = 0,
		Sphere = 1,
		// AP's cuboid: 0.3 x 0.3 x 1.0 m, OBB so it tumbles, cube inertia.
		// APPENDED, never renumbered - this is a wire value, and Cube's AABB volume
		// and sphere inertia are what every measurement in E1-E8 used.
		Cuboid = 2
	};
```

- [ ] **Step 2: Add the builder**

In `DistributedGameServer/ServerWorldManager.h`, next to the `AddCubeToWorld` declaration:

```cpp
			CSC8503::GameObject* AddCuboidToWorld(const CSC8503::Transform& transform, int count, int playerID) const;
```

In `DistributedGameServer/ServerWorldManager.cpp`, add the definition immediately after `AddCubeToWorld`'s definition. Read `AddCubeToWorld` first and mirror its structure — network object registration, world insertion and pool bookkeeping must match it exactly; only the volume, scale and inertia differ:

```cpp
CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::AddCuboidToWorld(
	const CSC8503::Transform& transform, int count, int playerID) const {
	// AP's cuboid is 0.3 x 0.3 x 1.0 m, so half-extents are (0.15, 0.15, 0.5).
	const Vector3 halfDims(0.15f, 0.15f, 0.5f);

	GameObject* cuboid = new GameObject(NoSpecialFeatures, "cuboid");

	// OBB, not AABB: an AABB cannot rotate, and AP's cuboids tumble under gravity
	// from a random initial velocity. OBB-OBB intersection is implemented.
	OBBVolume* volume = new OBBVolume(halfDims);
	cuboid->SetBoundingVolume((CollisionVolume*)volume);

	cuboid->GetTransform()
		.SetScale(halfDims * 2)
		.SetPosition(transform.GetPosition())
		.SetOrientation(transform.GetOrientation());

	cuboid->SetPhysicsObject(new PhysicsObject(&cuboid->GetTransform(), cuboid->GetBoundingVolume()));
	cuboid->GetPhysicsObject()->SetInverseMass(0.5f);
	// Cube inertia, not sphere: AddCubeToWorld uses InitSphereInertia and is left
	// alone because E1-E8 measured it, but a new archetype has no such constraint.
	cuboid->GetPhysicsObject()->InitCubeInertia();

	return cuboid;
}
```

> **Implementer note.** `AddCubeToWorld` may do more than the above — network object creation, `mCreatedObjectPool` insertion, `mGameWorld->AddGameObject`. **Read it and mirror everything it does**, changing only the volume type, the scale and the inertia. If it returns an object the caller then registers, do the same. Do not invent a different lifecycle.

- [ ] **Step 3: Dispatch on it**

In `CreateObjectFromArchetype`, replace the two-way ternary with a three-way selection:

```cpp
	GameObject* object = nullptr;
	switch (static_cast<NCL::Interaction::ObjectArchetype>(archetypeID)) {
	case NCL::Interaction::ObjectArchetype::Sphere:
		object = AddSphereToWorld(transform, networkID, playerID);
		break;
	case NCL::Interaction::ObjectArchetype::Cuboid:
		object = AddCuboidToWorld(transform, networkID, playerID);
		break;
	default:
		// Cube, and any unknown id: unchanged fallback, so an archetype a peer knows
		// and we do not still produces an object rather than dropping a handoff.
		object = AddCubeToWorld(transform, networkID, playerID);
		break;
	}
```

Adapt the surrounding lines to whatever local names the existing code uses.

- [ ] **Step 4: Build and verify nothing regressed**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:EntryPointManager`;EntryPointMidware`;EntryPointServer /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
```

Expected: all three roles link; `128 passed, 0 failed.` Nothing constructs a Cuboid yet, so behaviour is unchanged — correct at this stage.

- [ ] **Step 5: Commit**

```bash
git add CSC8503CoreClasses/DistributedSystemCommonFiles/InteractionCommand.h DistributedGameServer/ServerWorldManager.h DistributedGameServer/ServerWorldManager.cpp
git commit -m "feat(archetypes): add a Cuboid archetype with an OBB volume"
```

---

### Task 3: The injection schedule, as a pure function

All the logic that can be wrong lives here. Task 4 is wiring.

**Files:**
- Create: `CSC8503CoreClasses/DistributedSystemCommonFiles/InjectionSchedule.h`
- Modify: `tools/InteractionTests/InjectionScheduleTests.cpp` (append)

**Interfaces:**
- Consumes: `ObjectArchetype::Sphere`, `ObjectArchetype::Cuboid` (Task 2).
- Produces:
  - `struct NCL::Distributed::InjectionDraw { int index; double dueSeconds; bool boundarySite; int archetypeID; float u, v, w; float velX, velY, velZ; }`
  - `InjectionDraw NCL::Distributed::DrawInjection(int index, uint32_t seed, double ratePerSecond)`
  - `constexpr double NCL::Distributed::AP_INJECTION_RATE = 160.0`

- [ ] **Step 1: Write the failing tests**

Append to `tools/InteractionTests/InjectionScheduleTests.cpp`:

```cpp
#include "DistributedSystemCommonFiles/InjectionSchedule.h"

using namespace NCL::Distributed;

TEST(InjectionDueTimeIsIndexOverRate) {
	CHECK(DrawInjection(0, 42, AP_INJECTION_RATE).dueSeconds == 0.0);
	CHECK(DrawInjection(160, 42, AP_INJECTION_RATE).dueSeconds == 1.0);
	CHECK(DrawInjection(9600, 42, AP_INJECTION_RATE).dueSeconds == 60.0);
}

TEST(InjectionAlternatesBetweenTheTwoSites) {
	// AP injects 50% near a boundary and 50% at region centre. Deterministic
	// alternation gives exactly 50/50 rather than approximately.
	CHECK(DrawInjection(0, 42, AP_INJECTION_RATE).boundarySite == true);
	CHECK(DrawInjection(1, 42, AP_INJECTION_RATE).boundarySite == false);
	CHECK(DrawInjection(2, 42, AP_INJECTION_RATE).boundarySite == true);
	CHECK(DrawInjection(3, 42, AP_INJECTION_RATE).boundarySite == false);
}

TEST(InjectionSiteAndArchetypeAreIndependent) {
	// The trap this test exists for: selecting BOTH on index%2 would put every
	// sphere at the boundary and every cuboid at the centre - perfectly
	// correlated, where AP draws type independently of site. Over four
	// consecutive indices every (site, archetype) combination must appear once.
	int sphereAtBoundary = 0, cuboidAtBoundary = 0;
	int sphereAtCentre = 0, cuboidAtCentre = 0;
	for (int i = 0; i < 4; ++i) {
		const InjectionDraw draw = DrawInjection(i, 42, AP_INJECTION_RATE);
		const bool isSphere =
			draw.archetypeID == static_cast<int>(NCL::Interaction::ObjectArchetype::Sphere);
		if (draw.boundarySite) {
			if (isSphere) { ++sphereAtBoundary; } else { ++cuboidAtBoundary; }
		}
		else {
			if (isSphere) { ++sphereAtCentre; } else { ++cuboidAtCentre; }
		}
	}
	CHECK(sphereAtBoundary == 1);
	CHECK(cuboidAtBoundary == 1);
	CHECK(sphereAtCentre == 1);
	CHECK(cuboidAtCentre == 1);
}

TEST(InjectionArchetypeIsOnlySphereOrCuboid) {
	// Capsules are a declared deviation - capsule-capsule collision is not
	// implemented - and Cube is reserved for the pre-seeded workloads.
	for (int i = 0; i < 64; ++i) {
		const int archetype = DrawInjection(i, 7, AP_INJECTION_RATE).archetypeID;
		const bool valid =
			archetype == static_cast<int>(NCL::Interaction::ObjectArchetype::Sphere) ||
			archetype == static_cast<int>(NCL::Interaction::ObjectArchetype::Cuboid);
		CHECK(valid);
	}
}

TEST(InjectionVelocityStaysInsideAPsDistribution) {
	// AP: uniform in (-10 < x < 10, -10 < y < 0, -10 < z < 10) m/s.
	for (int i = 0; i < 512; ++i) {
		const InjectionDraw draw = DrawInjection(i, 99, AP_INJECTION_RATE);
		CHECK(draw.velX > -10.0f && draw.velX < 10.0f);
		CHECK(draw.velY > -10.0f && draw.velY <= 0.0f);
		CHECK(draw.velZ > -10.0f && draw.velZ < 10.0f);
	}
}

TEST(InjectionPositionFractionsAreUnitRange) {
	for (int i = 0; i < 256; ++i) {
		const InjectionDraw draw = DrawInjection(i, 3, AP_INJECTION_RATE);
		CHECK(draw.u >= 0.0f && draw.u < 1.0f);
		CHECK(draw.v >= 0.0f && draw.v < 1.0f);
		CHECK(draw.w >= 0.0f && draw.w < 1.0f);
	}
}

TEST(InjectionDrawIsPureInIndexAndSeed) {
	// Every server walks the same schedule independently, so a draw must not
	// depend on call order or on which server evaluates it.
	const InjectionDraw first = DrawInjection(1234, 42, AP_INJECTION_RATE);
	DrawInjection(1, 42, AP_INJECTION_RATE);
	DrawInjection(9999, 7, AP_INJECTION_RATE);
	const InjectionDraw again = DrawInjection(1234, 42, AP_INJECTION_RATE);
	CHECK(first.velX == again.velX);
	CHECK(first.velY == again.velY);
	CHECK(first.velZ == again.velZ);
	CHECK(first.u == again.u);
	CHECK(first.archetypeID == again.archetypeID);
	CHECK(first.boundarySite == again.boundarySite);
}

TEST(InjectionSeedChangesVelocitiesButNotTheSiteSchedule) {
	const InjectionDraw a = DrawInjection(5, 1, AP_INJECTION_RATE);
	const InjectionDraw b = DrawInjection(5, 2, AP_INJECTION_RATE);
	CHECK(a.boundarySite == b.boundarySite);
	CHECK(a.archetypeID == b.archetypeID);
	CHECK(a.dueSeconds == b.dueSeconds);
	// Velocities must actually differ, or the seed is not reaching the generator.
	const bool differs = (a.velX != b.velX) || (a.velY != b.velY) || (a.velZ != b.velZ);
	CHECK(differs);
}
```

- [ ] **Step 2: Run to verify it fails**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
```

Expected: **compile error** — `Cannot open include file: 'DistributedSystemCommonFiles/InjectionSchedule.h'`.

- [ ] **Step 3: Write the schedule**

Create `CSC8503CoreClasses/DistributedSystemCommonFiles/InjectionSchedule.h`:

```cpp
#pragma once
#include <cstdint>

#include "InteractionCommand.h"

// The spawn schedule for the AP-comparable injection benchmark.
//
// Aura Projection (Brown, Ushaw & Morgan, I3D 2019) injects 160 objects/second
// for 60 s: 50% into a volume near a region boundary, 50% into one at region
// centre, with types drawn at random and velocities uniform in
// (-10 < x < 10, -10 < y < 0, -10 < z < 10) m/s.
//
// This is a PURE function of (index, seed) with no state, and that is the point:
// every server walks the same schedule and spawns only the objects whose site
// falls inside its own region. No coordination, no central allocator, no
// duplicates - and no server owning the entire population at t=0, which a single
// designated spawner would cause and which would measure a handoff storm rather
// than the benchmark.
namespace NCL::Distributed {

	// AP's published rate. 160/s for 60 s is ~9,600 objects.
	constexpr double AP_INJECTION_RATE = 160.0;

	struct InjectionDraw {
		int index = 0;
		double dueSeconds = 0.0;
		// true = AP's site A, a volume near a region boundary.
		// false = AP's site B, a volume at the region centre.
		bool boundarySite = true;
		int archetypeID = 0;
		// Position within the chosen volume, each in [0, 1). The caller maps these
		// onto its own region, because region geometry is not knowable here.
		float u = 0.f, v = 0.f, w = 0.f;
		float velX = 0.f, velY = 0.f, velZ = 0.f;
	};

	namespace Detail {
		// SplitMix64. A counter-based mixer rather than a seeded engine, because an
		// engine carries state and would make a draw depend on how many draws
		// preceded it - which is exactly what must not happen when several servers
		// evaluate the same schedule independently.
		inline uint64_t Mix(uint64_t x) {
			x += 0x9E3779B97F4A7C15ull;
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			return x ^ (x >> 31);
		}

		// [0, 1) from the top 24 bits, which are the best-mixed.
		inline float UnitFloat(uint64_t bits) {
			return static_cast<float>((bits >> 40) & 0xFFFFFFull) / 16777216.0f;
		}

		inline float Draw(uint64_t seed, int index, int stream) {
			const uint64_t key = Mix(static_cast<uint64_t>(seed) * 0x100000000ull
				+ static_cast<uint64_t>(index) * 8ull + static_cast<uint64_t>(stream));
			return UnitFloat(key);
		}
	}

	inline InjectionDraw DrawInjection(int index, uint32_t seed, double ratePerSecond) {
		InjectionDraw draw;
		draw.index = index;
		draw.dueSeconds = (ratePerSecond > 0.0)
			? static_cast<double>(index) / ratePerSecond : 0.0;

		// Site and archetype must NOT share a divisor. Selecting both on index%2
		// would put every sphere at the boundary and every cuboid at the centre -
		// perfectly correlated, where AP draws type independently of site. index%2
		// for the site and (index/2)%2 for the type walks all four combinations in
		// turn, so each site receives an equal mix of both types.
		draw.boundarySite = (index % 2) == 0;
		draw.archetypeID = (((index / 2) % 2) == 0)
			? static_cast<int>(NCL::Interaction::ObjectArchetype::Sphere)
			: static_cast<int>(NCL::Interaction::ObjectArchetype::Cuboid);

		draw.u = Detail::Draw(seed, index, 0);
		draw.v = Detail::Draw(seed, index, 1);
		draw.w = Detail::Draw(seed, index, 2);

		// AP's distribution. y is strictly downward, so injected objects are already
		// falling rather than being lobbed upward.
		draw.velX = Detail::Draw(seed, index, 3) * 20.0f - 10.0f;
		draw.velY = Detail::Draw(seed, index, 4) * -10.0f;
		draw.velZ = Detail::Draw(seed, index, 5) * 20.0f - 10.0f;
		return draw;
	}
}
```

- [ ] **Step 4: Run to verify it passes**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
```

Expected: `136 passed, 0 failed.` (128 + 8 new), exit 0.

- [ ] **Step 5: Commit**

```bash
git add CSC8503CoreClasses/DistributedSystemCommonFiles/InjectionSchedule.h tools/InteractionTests/InjectionScheduleTests.cpp
git commit -m "feat(injection): deterministic AP spawn schedule"
```

---

### Task 4: Wire `--workload injection`

**Files:**
- Modify: `DistributedGameServer/ServerWorldManager.h` (injection state + declarations)
- Modify: `DistributedGameServer/ServerWorldManager.cpp` (skip pre-seeding; per-tick spawn; velocity hook at `:345`)

**Interfaces:**
- Consumes: `DrawInjection`, `InjectionDraw`, `AP_INJECTION_RATE` (Task 3); `SpawnObject(int archetypeID, const Maths::Vector3& at, int spawnerPlayerID)`, `GetObjectServer(pos)`, `GetWorldExtent(minX, maxX, minZ, maxZ)` (existing).
- Produces: `--workload injection` behaviour. No new public API.

- [ ] **Step 1: Skip pre-seeding for the injection workload**

The injection benchmark starts from an **empty world**. Find the world-construction block that dispatches on `mWorkload` (around `ServerWorldManager.cpp:1196-1263`, ending in the `CreateObjectGrid(...)` call) and return before it builds a grid:

```cpp
	// The injection workload starts EMPTY and accumulates over the run - that is
	// the whole shape of AP's benchmark. Pre-seeding a grid here would put the
	// full population in place at t=0 and measure a different experiment.
	if (mWorkload == "injection") {
		std::cout << "Injection workload: starting with an empty world.\n";
		return;
	}
```

Place it so it runs before any grid construction but after whatever bookkeeping the other workloads also require. Read the surrounding function and put it at the earliest point where returning leaves the server in a valid started state.

- [ ] **Step 2: Add injection state to the header**

In `DistributedGameServer/ServerWorldManager.h`, next to the other workload state:

```cpp
			// AP-comparable injection. mInjectionIndex is the next index in the global
			// schedule this server has yet to consider; every server walks the same
			// schedule and spawns only the objects landing in its own region, so the
			// counter advances identically everywhere.
			int mInjectionIndex = 0;
			double mInjectionElapsedSeconds = 0.0;
			void UpdateInjection(float dt);
			// Maps a schedule draw onto a world position inside this server's region.
			Maths::Vector3 InjectionPosition(const NCL::Distributed::InjectionDraw& draw) const;
```

Add `#include "DistributedSystemCommonFiles/InjectionSchedule.h"` to the header's include block.

- [ ] **Step 3: Implement the per-tick injection**

Add to `ServerWorldManager.cpp`, and call `UpdateInjection(dt)` from `ServerWorldManager::Update` immediately **before** `FlushScheduledHandoffs()` so a spawned object joins this tick's simulation:

```cpp
void DistributedGameServer::ServerWorldManager::UpdateInjection(float dt) {
	if (mWorkload != "injection") {
		return;
	}
	mInjectionElapsedSeconds += static_cast<double>(dt);

	// Walk the schedule to the current time. Every server runs this identically;
	// the region test below is what makes each object spawn exactly once.
	while (true) {
		const NCL::Distributed::InjectionDraw draw = NCL::Distributed::DrawInjection(
			mInjectionIndex, static_cast<uint32_t>(mSeed), NCL::Distributed::AP_INJECTION_RATE);
		if (draw.dueSeconds > mInjectionElapsedSeconds) {
			break;
		}
		++mInjectionIndex;

		const Maths::Vector3 at = InjectionPosition(draw);
		// Only the owning server spawns. SpawnObject would build a deactivated
		// object and burn a runtime id on every other server otherwise.
		if (GetObjectServer(at) != mServerID) {
			continue;
		}
		const int networkID = SpawnObject(draw.archetypeID, at, -1);
		if (networkID < 0) {
			continue;
		}
		const auto entry = mCreatedObjectPool.find(networkID);
		if (entry == mCreatedObjectPool.end() || entry->second == nullptr) {
			continue;
		}
		if (auto* physics = entry->second->GetPhysicsObject()) {
			physics->SetLinearVelocity(Maths::Vector3(draw.velX, draw.velY, draw.velZ));
		}
	}
}
```

> **Implementer note.** `mSeed` is the name assumed here for whatever member holds the `--seed` value. Find the real one (`grep -n "seed" DistributedGameServer/ServerWorldManager.h`) and use it. If the seed is not stored on the world manager, store it when `--seed` is applied — the schedule must be seed-driven or runs are not reproducible.

- [ ] **Step 4: Map a draw onto a region position**

```cpp
Maths::Vector3 DistributedGameServer::ServerWorldManager::InjectionPosition(
	const NCL::Distributed::InjectionDraw& draw) const {
	// AP's two injection sites, per the paper's section 5:
	//   site A - 20 x 20 x 150 m, centred 12 m from a boundary, 15 m above ground
	//   site B - 20 x 20 x 20 m at region centre, 15 m above ground
	// The 150 m dimension runs PARALLEL to the boundary (along Z here), matching
	// their Fig. 4 where boundary volumes are long thin rectangles along it.
	constexpr float SPAWN_HEIGHT = 15.0f;
	constexpr float SITE_A_THICKNESS = 20.0f;   // perpendicular to the boundary, X
	constexpr float SITE_A_LENGTH = 150.0f;     // parallel to the boundary, Z
	constexpr float SITE_A_DISTANCE = 12.0f;    // centre offset from the boundary
	constexpr float SITE_B_EXTENT = 20.0f;
	constexpr float SITE_HEIGHT = 20.0f;

	float minX = 0.f, maxX = 0.f, minZ = 0.f, maxZ = 0.f;
	if (mServerBorderMap != nullptr) {
		const auto mine = mServerBorderMap->find(mServerID);
		if (mine != mServerBorderMap->end() && mine->second != nullptr) {
			minX = mine->second->minXVal;
			maxX = mine->second->maxXVal;
			minZ = mine->second->minZVal;
			maxZ = mine->second->maxZVal;
		}
	}

	const float centreX = (minX + maxX) * 0.5f;
	const float centreZ = (minZ + maxZ) * 0.5f;
	const float y = SPAWN_HEIGHT + draw.v * SITE_HEIGHT;

	if (!draw.boundarySite) {
		return Maths::Vector3(
			centreX + (draw.u - 0.5f) * SITE_B_EXTENT,
			y,
			centreZ + (draw.w - 0.5f) * SITE_B_EXTENT);
	}

	// Site A sits inside this region, SITE_A_DISTANCE in from its maxX boundary.
	// Using maxX consistently (rather than whichever boundary is nearer) keeps the
	// site deterministic and identical on every server that evaluates it.
	const float siteCentreX = maxX - SITE_A_DISTANCE;
	return Maths::Vector3(
		siteCentreX + (draw.u - 0.5f) * SITE_A_THICKNESS,
		y,
		centreZ + (draw.w - 0.5f) * SITE_A_LENGTH);
}
```

> **Implementer note.** `mServerBorderMap` and the `minXVal`/`maxXVal`/`minZVal`/`maxZVal` field names are taken from `TakeLoadReport`, which reads the same map. Verify them and adapt. If a server's region is narrower than `SITE_A_DISTANCE`, `siteCentreX` can fall outside the region — the `GetObjectServer(at) != mServerID` test in Step 3 then simply skips it, which is correct behaviour rather than a bug, but note it in your report if it happens at the configured server count.

- [ ] **Step 5: Build, verify, and smoke-test**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:EntryPointManager`;EntryPointMidware`;EntryPointServer /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
& $msb DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
$built = "EntryPoint\Release"
$map = @{ "EntryPointManager" = "Manager"; "EntryPointMidware" = "Midware"; "EntryPointServer" = "DistributedPhysicsServer" }
foreach ($k in $map.Keys) { Copy-Item (Join-Path $built "$k.exe") (Join-Path "deploy\$($map[$k])" "EntryPoint.exe") -Force }
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 -Name inject-smoke -Sweep servers -Values 2 -Repeats 1 `
  -Objects 0 -Ticks 0 -Seconds 10 -Seed 42 -Workload injection -World "-150,150,-150,150" -DrainSeconds 0
```

Expected: `136 passed, 0 failed.`; the run completes with 2/2 servers; and roughly 1,600 objects exist after 10 s at 160/s. Check with:

```bash
grep -ho "objSpawned=[0-9]*" runs/exp-inject-smoke/servers2-r1/mid.log | tail -2
```

The two servers' `objSpawned` should sum to approximately 1,600. A sum near zero means the region test is rejecting every site; a sum near 3,200 means both servers are spawning every object and the region test is not working.

- [ ] **Step 6: Commit**

```bash
git add DistributedGameServer/ServerWorldManager.h DistributedGameServer/ServerWorldManager.cpp
git commit -m "feat(injection): --workload injection spawning on the AP schedule"
```

---

### Task 5: The `substeps` metric column

**Files:**
- Modify: `CSC8503CoreClasses/DistributedSystemCommonFiles/MetricSink.h` (`TickSample`, CSV header, CSV row)
- Modify: `DistributedGameServer/ServerWorldManager.cpp` (populate it)

**Interfaces:**
- Consumes: nothing.
- Produces: a `substeps` column in `ticks-server<N>.csv`, consumed by Task 6.

- [ ] **Step 1: Add the field**

In `MetricSink.h`, add to `TickSample` next to `physicsMs`:

```cpp
		// Physics substeps executed during this loop iteration.
		//
		// A sample is recorded on EVERY loop iteration, and in realtime mode the
		// loop spins at roughly 1 kHz while physics substeps at 120 Hz - so most
		// rows did no physics at all. AP's frame time is an update period, so the
		// analysis needs to know which iterations were real frames. Keying off
		// physicsMs > 0 is not sufficient: it rounds to zero.
		int32_t substeps = 0;
```

Add `substeps` to the CSV header string and the row writer in the same file, keeping the column order consistent between the two. Append it at the end of the row so existing column positions are unchanged.

- [ ] **Step 2: Populate it**

In `ServerWorldManager::Update`, where the other `sample.*` fields are assigned, add:

```cpp
		sample.substeps = static_cast<int32_t>(Profiler::GetPhysicsSubsteps());
```

> **Implementer note.** `Profiler::GetPhysicsSubsteps()` may not exist. Check (`grep -n "Substep\|substep" CSC8503CoreClasses/PhysicsSystem.cpp CSC8503CoreClasses/Profiler.h`). `PhysicsSystem::Update` runs an inner substep loop; if no counter is exposed, add one the same way the other Profiler counters are exposed — set it in `PhysicsSystem::Update` and read it here. Do not infer the count from timings.

- [ ] **Step 3: Build and verify the column appears**

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:EntryPointManager`;EntryPointMidware`;EntryPointServer /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
$built = "EntryPoint\Release"
$map = @{ "EntryPointManager" = "Manager"; "EntryPointMidware" = "Midware"; "EntryPointServer" = "DistributedPhysicsServer" }
foreach ($k in $map.Keys) { Copy-Item (Join-Path $built "$k.exe") (Join-Path "deploy\$($map[$k])" "EntryPoint.exe") -Force }
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 -Name substep-smoke -Sweep servers -Values 1 -Repeats 1 `
  -Objects 100 -Ticks 0 -Seconds 5 -Seed 42 -Workload uniform -World "-150,150,-150,150" -DrainSeconds 0
```

Then:

```bash
head -1 runs/exp-substep-smoke/servers1-r1/ticks-server0.csv
awk -F, 'NR>1 && $NF>0 {n++} END {print "rows with substeps>0:", n}' runs/exp-substep-smoke/servers1-r1/ticks-server0.csv
```

Expected: the header ends with `substeps`, and a realtime run has a **minority** of rows with `substeps > 0` — that is the documented ~1 kHz loop against a 120 Hz substep rate. All rows non-zero would mean the counter is not being reset per iteration.

- [ ] **Step 4: Confirm the existing suite still passes**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
.\tools\InteractionTests\Release\InteractionTests.exe
python -m unittest discover -s tools -p "test_analyse.py"
```

Expected: `136 passed, 0 failed.` and 19 Python tests OK.

- [ ] **Step 5: Commit**

```bash
git add CSC8503CoreClasses/DistributedSystemCommonFiles/MetricSink.h DistributedGameServer/ServerWorldManager.cpp CSC8503CoreClasses/PhysicsSystem.cpp CSC8503CoreClasses/PhysicsSystem.h
git commit -m "feat(metrics): record physics substeps per loop iteration"
```

(Drop any of those paths you did not actually change.)

---

### Task 6: AP frame-time report in `analyse.py`

**Files:**
- Modify: `tools/analyse.py`
- Modify: `tools/test_analyse.py`

**Interfaces:**
- Consumes: the `substeps` CSV column (Task 5), the existing `time_us` column.
- Produces: `ap_frame_times(rows, bucket_seconds=5.0)` and a printed `AP FRAME TIME` section.

- [ ] **Step 1: Write the failing tests**

Append to `tools/test_analyse.py`:

```python
class ApFrameTimeTests(unittest.TestCase):
    """AP's metric: max frame time across servers per 5 s window.

    A 'frame' is a loop iteration in which physics advanced; its frame time is
    the wall-clock gap since the previous such iteration. Rows with substeps == 0
    did no physics and are not frames - but the time they consumed still belongs
    to the next frame's interval, which is why the gap is measured between
    consecutive physics rows rather than between adjacent CSV rows.
    """

    def test_gap_is_measured_between_physics_rows_not_adjacent_rows(self):
        rows = [
            {"time_us": "0", "substeps": "1"},
            {"time_us": "1000", "substeps": "0"},
            {"time_us": "2000", "substeps": "0"},
            {"time_us": "8000", "substeps": "1"},
        ]
        buckets = analyse.ap_frame_times(rows, bucket_seconds=5.0)
        # One frame interval: 8000 - 0 = 8 ms. The two idle rows are absorbed.
        self.assertEqual(buckets, {0: 8.0})

    def test_max_is_taken_within_a_bucket(self):
        rows = [
            {"time_us": "0", "substeps": "1"},
            {"time_us": "2000", "substeps": "1"},
            {"time_us": "9000", "substeps": "1"},
        ]
        buckets = analyse.ap_frame_times(rows, bucket_seconds=5.0)
        self.assertEqual(buckets, {0: 7.0})

    def test_frames_are_bucketed_by_elapsed_time(self):
        rows = [
            {"time_us": "0", "substeps": "1"},
            {"time_us": "1000", "substeps": "1"},
            {"time_us": "6000000", "substeps": "1"},
        ]
        buckets = analyse.ap_frame_times(rows, bucket_seconds=5.0)
        self.assertIn(0, buckets)
        self.assertIn(1, buckets)

    def test_no_physics_rows_yields_no_buckets(self):
        rows = [{"time_us": "0", "substeps": "0"}, {"time_us": "1000", "substeps": "0"}]
        self.assertEqual(analyse.ap_frame_times(rows, bucket_seconds=5.0), {})

    def test_a_single_physics_row_yields_no_interval(self):
        rows = [{"time_us": "5", "substeps": "1"}]
        self.assertEqual(analyse.ap_frame_times(rows, bucket_seconds=5.0), {})

    def test_missing_substeps_column_is_treated_as_no_frames(self):
        # Runs recorded before the substeps column existed must not silently
        # produce a frame-time series computed from every loop iteration.
        rows = [{"time_us": "0"}, {"time_us": "1000"}]
        self.assertEqual(analyse.ap_frame_times(rows, bucket_seconds=5.0), {})
```

- [ ] **Step 2: Run to verify it fails**

```
python -m unittest discover -s tools -p "test_analyse.py"
```

Expected: `AttributeError: module 'analyse' has no attribute 'ap_frame_times'`.

- [ ] **Step 3: Implement it**

Add to `tools/analyse.py`, next to the other per-run analysis helpers:

```python
def ap_frame_times(rows, bucket_seconds=5.0):
    """Max frame time (ms) per elapsed-time bucket, AP's metric.

    AP aggregates "the maximum frame time of any server" over each 5 s period.
    This computes one server's per-bucket maximum; the caller takes the max
    across servers and then the mean across repeats.

    A frame is a loop iteration in which physics advanced (substeps > 0). Its
    frame time is the wall-clock gap since the previous such iteration, so the
    idle spin between them is charged to the frame that follows it - which is
    what AP's update period actually contains.
    """
    stamps = []
    for row in rows:
        raw = row.get("substeps")
        if raw is None:
            # Recorded before the column existed. Returning nothing is deliberate:
            # deriving frames from every loop iteration would report the ~1 kHz
            # spin rate as a frame rate.
            return {}
        try:
            if int(raw) <= 0:
                continue
            stamps.append(int(row["time_us"]))
        except (TypeError, ValueError):
            continue

    if len(stamps) < 2:
        return {}

    start = stamps[0]
    buckets = {}
    for previous, current in zip(stamps, stamps[1:]):
        frame_ms = (current - previous) / 1000.0
        bucket = int(((current - start) / 1e6) // bucket_seconds)
        if frame_ms > buckets.get(bucket, 0.0):
            buckets[bucket] = frame_ms
    return buckets
```

- [ ] **Step 4: Run to verify it passes**

```
python -m unittest discover -s tools -p "test_analyse.py"
```

Expected: 25 tests OK (19 existing + 6 new).

- [ ] **Step 5: Wire it into the report**

Add an `AP FRAME TIME` section to the experiment report, printed only when at least one run produces buckets. For each swept point, take the per-bucket maximum across every server in a repeat, then the mean of those maxima across repeats, and print one row per bucket:

```
AP FRAME TIME (max across servers per 5s window, mean over repeats)
  servers  window_s  mean_max_frame_ms
        1       0-5              4.812
        1      5-10              6.207
```

Keep it out of `INVARIANT FAILURES` — it is a measurement, not a check, and must not affect the exit code.

- [ ] **Step 6: Commit**

```bash
git add tools/analyse.py tools/test_analyse.py
git commit -m "feat(analyse): AP-comparable frame-time report"
```

---

### Task 7: Phase 1 validation and results

**Files:**
- Create: `docs/superpowers/results/2026-08-21-AP-injection.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the Phase 1 record, and the machine baseline that Phase 2 needs.

- [ ] **Step 1: Rebuild and stage**

```powershell
Set-Location "C:\Users\erendegirmenci\Desktop\Projects\Distributed-Physics-Server-Simulation"
$msb = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msb DistributedPhysicsSystem.sln /t:EntryPointManager`;EntryPointMidware`;EntryPointServer /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
$built = "EntryPoint\Release"
$map = @{ "EntryPointManager" = "Manager"; "EntryPointMidware" = "Midware"; "EntryPointServer" = "DistributedPhysicsServer" }
foreach ($k in $map.Keys) { Copy-Item (Join-Path $built "$k.exe") (Join-Path "deploy\$($map[$k])" "EntryPoint.exe") -Force }
```

- [ ] **Step 2: Run the AP benchmark at 1 and 2 servers**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 -Name ap-injection -Sweep servers -Values 1,2 -Repeats 5 `
  -Objects 0 -Ticks 0 -Seconds 60 -Seed 42 -Workload injection -World "-150,150,-150,150" `
  -HaloWidth 8 -HaloLookahead 4 -HaloReliable -HandoffLookahead 300 -DrainSeconds 0
```

Run it **synchronously in the foreground**. Ten runs of 60 s plus bring-up is roughly 15 minutes.

- [ ] **Step 3: Read the results**

```
python tools/analyse.py runs/exp-ap-injection
```

Record: the `AP FRAME TIME` table for both server counts; total objects spawned (should approach 9,600 across servers); and the custody and conservation counters. **The run crosses the ~3,384 objects/server correctness budget around t≈42 s at 2 servers**, so expect `hoResent`, `hoReclaimed`, `hoCustody` and a non-zero `conservation_delta`. Report them as a result; they are the benchmark stressing the system past its measured budget, which is worth knowing.

- [ ] **Step 4: Write the results document**

Create `docs/superpowers/results/2026-08-21-AP-injection.md` covering:
- AP's published parameters (from the spec's §1) and the measured configuration beside them.
- The `AP FRAME TIME` table, and AP's own Fig. 5 figures for reference (1 server ≈ 50 ms by 60 s; 10 servers ≈ 5 ms).
- **The declared-deviation table from the spec's §6, in full.** Without it "AP-comparable" is unsupported.
- A plain statement that Phase 1 is single-machine and therefore **cannot** reproduce AP's scalability claim, because adding servers on one box adds contention. Phase 1 validates the workload, the metric and the per-machine baseline.
- The custody/conservation behaviour past the correctness budget.
- The §1.1 observation that this project's halo bound is a zero-latency special case of AP's aura formula.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/results/2026-08-21-AP-injection.md
git commit -m "docs: Phase 1 AP-comparable injection results"
```

---

## Notes for the executor

- **`runs/` is gitignored.** Only the results document is committed.
- **Do not modify `Cube = 0`.** Its `AABBVolume` and sphere inertia are what E1–E8 measured.
- **Do not change the physics substep rate** to match AP's 16 ms. It would invalidate every prior measurement; the difference is a declared deviation.
- **Task 4 Step 5's object-count check is the real gate** for the injection workload. A sum near zero or near double means the per-server region test is wrong, and every later number would be meaningless.
- **Do not push or merge.** The branch is many commits ahead of origin and the user has not asked for either.
