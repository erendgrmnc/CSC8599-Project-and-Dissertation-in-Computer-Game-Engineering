# Phase D — Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop an object ever being owned by nobody or by two servers at once — close the ownership gap that release-on-send makes the default, fix and wire the arrival clamp, and stop custody resends duplicating objects — then measure all three at 4 servers, where two of them are only reachable.

**Architecture:** Three defects share one root: ownership is decided from *object state* at the moment a packet lands, and object state cannot distinguish the cases that matter. Item 2 gives the exchange a shared tick so release and install happen on the same one. Item 15 gives each transfer an identity, `(senderServerID, mSenderTick)`, so a resend of a transfer already accepted is recognisable as one even after the object has moved on — both fields are already on the wire, so there is **no wire-format change**. Item 7 makes the arrival clamp agree with `OwningServerFor` before it is allowed to move anything. Every fix is a pure predicate in a shared header with a Tier-0 test, in the pattern `HandoffCustody.h` and `RegionOwnership.h` already establish.

**Tech Stack:** C++20 / MSVC x64; `tools/InteractionTests` (dependency-free `TEST`/`CHECK` harness, sources listed in `tools/InteractionTests/CMakeLists.txt`); `tools/run-experiments.ps1` + `tools/analyse.py` + `tools/gate-compare.py` (stdlib Python).

**Spec:** `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` §5. **The spec is incomplete for this phase** and Task 8 corrects it: §5.2 bundles items 7 and 2 only, but item 15 (found 2026-08-24, `docs/superpowers/results/2026-08-24-B-e4-attribution.md`) was assigned to Phase D by the roadmap status pass and has no design section, no gate coverage and no re-run entry. §5.8 is written in Task 8 from what this plan implements.

## Global Constraints

- Every measurement run passes `--fixed-step` and `--seed 42`. Both are forwarded by the **midware**, not the game server.
- `--handoff-delay-ticks` is fault injection and must be **0** in every run in this plan.
- Correctness runs are paced: `-Ticks N`, never `-Seconds`. Performance is not claimed in this phase.
- Any run intended to pass a gate is drained until `hoCustody` reads 0. Phase B measured the 5 s default leaving 315 in custody on the rebalancing configuration and needing 120 s: use `-DrainSeconds 120` on rebalancing runs.
- `analyse.py`'s `check_custody` must not be weakened to make a run pass (§5.6).
- `-Values` is a comma-separated **string**. `-Values 1,2` as an array arrives as `12`.
- Release builds only: `tools\build-deploy.ps1 -Config Release`. The script defaults to Debug.
- New test sources must be added to `tools/InteractionTests/CMakeLists.txt`, not just to disk.
- Commit messages carry no `Co-Authored-By` trailer.

## Controller rulings taken while writing this plan

Recorded here in the Phase A phase-log pattern, because each changes what the evidence can support.

| Ruling | Decision | Cost if wrong |
|---|---|---|
| D1 | **The §5.6 gate is split in two.** §5.6 asks that at an explicit `--handoff-lookahead 0` a paced run reproduce the pre-change baseline exactly — but wiring the clamp (item 7) deliberately moves incoming objects *at lookahead 0*, so that gate cannot hold across the whole phase. Gate A runs after Tasks 1 and 3 (both no-ops on a healthy-path run) and must reproduce exactly. Tasks 2 and 5 are then measured **against Gate A** as intended changes, not gated to equality. | If Task 3's predicate is not in fact a no-op on the healthy path, Gate A catches it — which is the point of taking it there. The residual risk is that Task 2's change is judged against a baseline one task old rather than the phase's start; the two are equal by Gate A's own result. |
| D2 | **Item 15's transfer identity is `(senderServerID, mSenderTick)`**, both already on `StartSimulatingObjectPacket`. No new field, no wire-format change, no version negotiation. | If two different transfers of the same object could ever share both values, a genuine handoff would be dropped as a duplicate — a loss, the failure this phase exists to remove. Task 3 Step 1 pins the cases that make this safe: a later transfer of the same object necessarily carries a later tick from that sender, and the sender id disambiguates servers that share no epoch. |
| D3 | **The 4-server axis is correctness-only** — conservation, ownership gap, double-owner, duplication. No timing figure is taken at 4 servers. | None to the claims. 4 servers + manager + midware + client is 7 processes on 6 cores, so a 4-server *timing* number measures contention (`docs/EVALUATION.md` §5 says so). State and event measures are immune, under the same carve-out §5 grants E1 and E2. |
| D4 | **The new default lookahead is derived by measurement in Task 5, not chosen here.** The sweep is L ∈ {0, 2, 4, 8, 16}; the default is the smallest L with `ownership_gap_ticks = 0` and `hoLate = 0` at both 2 and 4 servers. | A number picked by argument would encode this machine's loopback RTT as a design constant. Deriving it records the criterion instead, so a redeployment can re-derive it. |
| D6 | **The 4-server correctness and gate runs use `uniform`, not `seam`.** This plan originally specified `seam` on the reasoning that it puts objects exactly on the interior Z seam. It does — and then leaves them there: `seam` falls through the velocity assignment at `ServerWorldManager.cpp:1159`, so a 4-server `seam` run produces `hoSent = hoRecv = hoClamp = 0` (measured, `runs/exp-d0-probe4`). It is a placement test, kept as Task 0 Step 4b. `uniform` moves and crosses both seams (measured: 92 handoffs, `hoClamp` 8, `ownership_gap_ticks` 105). | Caught before execution rather than after. Had it stood, Gate A at 4 servers would have compared two runs with the entire handoff path switched off and passed — the same shape as item 14, where invariant I4 passed vacuously in every run of a phase because no client ever printed `@@FINAL`. |
| D5 | **Item 2 is bounded by the halo band, not only by RTT.** A lookahead of L ticks means the sender simulates the object for L ticks after it has left the sender's region, so it must still be inside the receiver's halo band: `v_max * L * dt <= halo_width`. Task 5 asserts this rather than discovering it in a sweep. | Without the bound, raising the default lookahead silently degrades cross-border fidelity — the object is integrated by a server that does not own its position and the owner has no shadow of it. This links item 2 to Phase C's bound, which no document currently does. |

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `CSC8503CoreClasses/DistributedSystemCommonFiles/RegionOwnership.h` | The single ownership rule | Add `ClampIntoRegion` beside `OwningServerFor` |
| `CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h` | Pure custody/arrival predicates | Add `AcceptedTransfer` + `IsResendOfAcceptedTransfer` |
| `DistributedGameServer/ServerWorldManager.cpp:2392` | `CalculateIncomingObjectOffsetPosition` | Delegate to `ClampIntoRegion`; correct two stale comments |
| `DistributedGameServer/ServerWorldManager.cpp:2069` | `ApplyIncomingObject` | Apply the clamp; consult the resend guard |
| `DistributedGameServer/ServerWorldManager.h` | `ServerWorldManager` state | Add `mAcceptedTransfers` |
| `DistributedGameServer/ServerStarter.cpp:113` | Flag parsing | New `--handoff-lookahead` default |
| `tools/InteractionTests/RegionOwnershipTests.cpp` | The ownership rule's tests | Add clamp cases beside the rule, reusing its existing `TwoServerWorld()`/`FourServerWorld()` fixtures |
| `tools/InteractionTests/HandoffCustodyTests.cpp` | Custody predicates | Add resend-identity cases |
| `docs/superpowers/specs/2026-08-23-backlog-completion-design.md` | Phase D design | Add §5.8 (item 15) |
| `docs/EVALUATION.md`, `docs/SPATIAL-PARTITIONING.md`, `CLAUDE.md` | §5.5's documentation table | Correct on completion |

---

### Task 0: Establish the step-0 baseline, at 2 **and** 4 servers

No stored dataset survives (spec §1.1), and this phase is the first to need a 4-server baseline at all.

**Files:** none modified. Produces `runs/exp-d0-base2/`, `runs/exp-d0-base4/`.

**Interfaces:**
- Produces: two experiment directories that Gate A (Task 4) compares against, and the first 4-server conservation numbers in the project.

- [ ] **Step 1: Record the commit the baseline is taken at**

```bash
git rev-parse HEAD | tee docs/superpowers/results/2026-08-26-D-baseline-commit.txt
git status --short   # must be empty; a dirty tree stamps every manifest irreproducible
```

- [ ] **Step 2: Build Release**

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
```

Expected: `deploy/Manager`, `deploy/Midware`, `deploy/Client`, `deploy/DistributedPhysicsServer` each hold an `EntryPoint*.exe` newer than the build start.

- [ ] **Step 3: Take the 2-server baseline, 4 repeats**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name d0-base2 -Sweep servers -Values "2" -Repeats 4 `
  -Objects 400 -Ticks 1800 -Seed 42 -Workload uniform `
  -HaloWidth 8 -HaloReliable -HandoffLookahead 0 -DrainSeconds 30
```

Four repeats, not three: Gate A compares 4 pre against 4 post, which is what Phase A's gate did.

- [ ] **Step 4: Take the 4-server baseline, 4 repeats**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name d0-base4 -Sweep servers -Values "4" -Repeats 4 `
  -Objects 400 -Ticks 1800 -Seed 42 -Workload uniform `
  -HaloWidth 8 -HaloReliable -HandoffLookahead 0 -DrainSeconds 30
```

`uniform`, **not** `seam` — see ruling D6. `uniform` is the only bundled workload that both spreads across the whole world and moves (`ServerWorldManager.cpp:1159`: everything except `shuttle` and `uniform` falls through the velocity assignment). At 4 servers `CalculateServerBorders` builds a 2×2 grid (`numCols = ceil(sqrt(4)) = 2`, `numRows = 2`), so both `x = 0` and `z = 0` are interior seams, and `uniform`'s ±10 units/s `lateralZ` spread (`SHUTTLE_Z_SPREAD = 20.0f`) carries objects across the Z one over a 15-second run.

Measured on this configuration, 2 repeats (`runs/exp-d0-probe4u`, 2026-08-26): 92 and 94 handoffs, `hoClamp` 5 and 3 on servers 0 and 2 in both repeats, conservation exact at 400, parity exact, `hoCustody` 0, `hoDup` 0 — and `ownership_gap_ticks` **105 and 102**, which `analyse.py` reports as an invariant failure (exit 1). That failure is the documented `--handoff-lookahead 0` behaviour this phase exists to remove, not a broken baseline: Phase A ruling P6 accepted the same thing at 2 servers (`ownership_gap_ticks = 84`).

- [ ] **Step 4b: Take the seam placement check, separately**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name d0-seam4 -Sweep servers -Values "4" -Repeats 2 `
  -Objects 400 -Ticks 1800 -Seed 42 -Workload seam `
  -HaloWidth 8 -HaloReliable -HandoffLookahead 0 -DrainSeconds 30
```

`seam` is a **placement** test, not a crossing test: it centres each grid on the world origin so a whole row and column sit exactly on `x = 0` and `z = 0`, then leaves them there. It answers "does the half-open rule assign a 2-D seam point to exactly one server", which nothing had ever checked at 4 servers, and it answers nothing about handoffs.

Measured 2026-08-26 (`runs/exp-d0-probe4`): `owned = 100` on each of the four servers, 400 total, `mismatch = 0` — the rule holds at a 2-D seam. And `hoSent = hoRecv = hoClamp = 0` on every server, which is why it cannot serve as the gate baseline.

- [ ] **Step 5: Analyse both, and expect the 4-server run to be the interesting one**

```powershell
python tools\analyse.py runs\exp-d0-base2
python tools\analyse.py runs\exp-d0-base4
```

Record, per experiment: `conservation_delta`, `ho_parity_delta`, `ownership_gap_ticks`, `ownership_double_ticks`, `hoClamp`, `hoDup`, `hoResent`, `hoCustody`.

**Do not treat a 4-server failure here as a blocker.** A non-zero `ownership_double_ticks` at 4 servers is a *finding*, not a broken baseline — it is the first time a 2-D seam has been measured, and it is what Task 1 and Task 5 exist to fix. Record it and continue. A non-zero `hoCustody` **is** a blocker: raise `-DrainSeconds` until it reads 0, and note the value needed.

- [ ] **Step 6: Commit the baseline record**

```bash
git add docs/superpowers/results/2026-08-26-D-baseline-commit.txt
git commit -m "docs(D): record the step-0 baseline commit and configuration"
```

---

### Task 1: Fix the arrival clamp's Z bound

Item 7, first half. Pure geometry, no run required — the spec's own observation (§5.3) that this is directly assertable is what makes it a unit-test task.

**Files:**
- Modify: `tools/InteractionTests/RegionOwnershipTests.cpp` (append; **no new file and no `CMakeLists.txt` change**)
- Modify: `CSC8503CoreClasses/DistributedSystemCommonFiles/RegionOwnership.h`
- Modify: `DistributedGameServer/ServerWorldManager.cpp:2378-2409`

**Interfaces:**
- Consumes: `NCL::Interaction::OwningServerFor`, `NCL::Interaction::RegionBounds`, and the existing fixtures `TwoServerWorld()` / `FourServerWorld()` already defined in the anonymous namespace at `RegionOwnershipTests.cpp:11-25`.
- Produces: `NCL::Interaction::ClampIntoRegion(const RegionBounds& region, float worldMaxX, float worldMaxZ, const Maths::Vector3& point) -> Maths::Vector3`. `ServerWorldManager::CalculateIncomingObjectOffsetPosition` becomes a thin adapter onto it, so the clamp and the ownership rule cannot drift apart again — the exact failure this task fixes.

**Why these tests go in the existing file.** `RegionOwnershipTests.cpp` already holds `TwoServerWorld()` and `FourServerWorld()` with exactly the partitions these tests need. A new file would duplicate them, and a second copy of a partition definition drifting from the first is precisely the defect being fixed. The clamp also now lives in `RegionOwnership.h`, so its tests belong beside the rule they must agree with.

- [ ] **Step 1: Write the failing tests**

Append to `tools/InteractionTests/RegionOwnershipTests.cpp`. The file already has `using namespace NCL::Interaction;` at file scope and its fixtures in an anonymous namespace, so these need no new includes:

```cpp
// --- ClampIntoRegion ------------------------------------------------------------
//
// The arrival clamp must agree with the rule above, not with a stale description of it.
// Until 2026-08-26 ServerWorldManager::CalculateIncomingObjectOffsetPosition kept its
// own copy of the bounds and clamped Z with an INCLUSIVE upper bound, matching what
// IsObjectInBorder did BEFORE the ownership unification. OwningServerFor is half-open on
// both axes, so on an interior Z seam that clamp returned a coordinate a DIFFERENT
// server owns - the disowned-object case the unification exists to prevent.
//
// Masked at 2 servers (1-D split, maxZ == worldMaxZ, so the closed-outer-edge exception
// applies) and live at 4 (CalculateServerBorders builds a 2x2 grid). That is why no run
// caught it: every conservation measurement to date ran at 2 servers.

TEST(ClampedPointOnAnInteriorZSeamIsOwnedByThisServer) {
	// THE REGRESSION TEST. Server 0's region is [-150,0) x [-150,0). An object arriving
	// at z = 0 sits exactly on the interior seam; the old clamp returned z = 0 unchanged,
	// and OwningServerFor gives z = 0 to server 2.
	const std::vector<RegionBounds> world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(-10.0f, 0.0f, 0.0f));
	CHECK(OwningServerFor(world, clamped) == 0);
}

TEST(ClampedPointPastAnInteriorZSeamIsOwnedByThisServer) {
	const std::vector<RegionBounds> world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(-10.0f, 0.0f, 7.5f));
	CHECK(OwningServerFor(world, clamped) == 0);
}

TEST(ClampedPointOnTheWorldOuterZEdgeStaysOnIt) {
	// The outer maximum is CLOSED - it belongs to the last row, or it would belong to
	// nobody. Clamping it inward would be wrong, not merely unnecessary.
	const std::vector<RegionBounds> world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[2], 150.0f, 150.0f, Maths::Vector3(-10.0f, 0.0f, 150.0f));
	CHECK(clamped.z == 150.0f);
	CHECK(OwningServerFor(world, clamped) == 2);
}

TEST(ClampedPointOnAnInteriorXSeamIsOwnedByThisServer) {
	// X was already correct. Pinned so the epsilon treatment is not lost while Z gains it.
	const std::vector<RegionBounds> world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(0.0f, 0.0f, -10.0f));
	CHECK(OwningServerFor(world, clamped) == 0);
}

TEST(ClampIsANoOpForAPointAlreadyInside) {
	const std::vector<RegionBounds> world = FourServerWorld();
	const Maths::Vector3 inside(-75.0f, 3.0f, -75.0f);
	const Maths::Vector3 clamped = ClampIntoRegion(world[0], 150.0f, 150.0f, inside);
	CHECK(clamped.x == inside.x);
	CHECK(clamped.y == inside.y);
	CHECK(clamped.z == inside.z);
}

TEST(ClampPreservesTheYCoordinate) {
	// The clamp is an XZ operation. A handoff that silently floored Y would drop objects
	// through the world.
	const std::vector<RegionBounds> world = FourServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(0.0f, 42.0f, 0.0f));
	CHECK(clamped.y == 42.0f);
}

TEST(TwoServerSplitIsUnaffectedByTheZFix) {
	// The 1-D case must not move: every conservation figure on record was measured on it,
	// and Gate A compares against those runs.
	const std::vector<RegionBounds> world = TwoServerWorld();
	const Maths::Vector3 clamped =
		ClampIntoRegion(world[0], 150.0f, 150.0f, Maths::Vector3(-10.0f, 0.0f, 150.0f));
	CHECK(clamped.z == 150.0f);
	CHECK(OwningServerFor(world, clamped) == 0);
}
```

- [ ] **Step 2: Run to verify they fail**

No CMake regenerate is needed — the file is already in the target.

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
```

Expected: **compile failure**, `'ClampIntoRegion': identifier not found`. That is the correct first failure — the function does not exist yet.

- [ ] **Step 3: Write the shared clamp**

Append to `CSC8503CoreClasses/DistributedSystemCommonFiles/RegionOwnership.h`, inside `namespace NCL::Interaction`, below `OwningServerFor`:

```cpp
	// Clamps a point strictly inside `region`, under the SAME rule OwningServerFor
	// applies - so ClampIntoRegion(r, ..., p) is always owned by r.serverId.
	//
	// It lives here, next to the rule, deliberately. It used to live in
	// ServerWorldManager as CalculateIncomingObjectOffsetPosition with its own copy of
	// the bounds, and the copy went stale: it clamped Z with an INCLUSIVE upper bound,
	// matching what IsObjectInBorder did before the ownership unification. On an
	// interior Z seam that returned a coordinate a different server owns - the
	// disowned-object case the unification exists to prevent, on the one path that had
	// not been unified. Two definitions of one rule is the defect; one definition is
	// the fix.
	//
	// worldMaxX / worldMaxZ carry the outer-edge exception: a region on the world
	// boundary owns its maximum, so clamping there must NOT step inward.
	inline Maths::Vector3 ClampIntoRegion(const RegionBounds& region,
		float worldMaxX, float worldMaxZ, const Maths::Vector3& point) {
		// One centimetre in world units - large enough to survive the float rounding
		// that put the object on the edge, far below the 2-unit object spacing.
		constexpr float INWARD_EPSILON = 0.01f;

		// An exclusive upper bound means the maximum itself is not a legal position, so
		// the reachable ceiling is one epsilon below it. A region whose maximum IS the
		// world's owns that maximum, so its ceiling is the maximum itself. std::max
		// guards the degenerate region where the epsilon would invert the range, which
		// would make std::clamp undefined.
		const float ceilingX = (region.maxX == worldMaxX)
			? region.maxX : std::max(region.minX, region.maxX - INWARD_EPSILON);
		const float ceilingZ = (region.maxZ == worldMaxZ)
			? region.maxZ : std::max(region.minZ, region.maxZ - INWARD_EPSILON);

		Maths::Vector3 clamped = point;
		clamped.x = std::clamp(point.x, region.minX, ceilingX);
		clamped.z = std::clamp(point.z, region.minZ, ceilingZ);
		// Y is untouched: the partition is in XZ only.
		return clamped;
	}
```

Add `#include <algorithm>` to the header's include block for `std::clamp` and `std::max`.

- [ ] **Step 4: Run the tests to verify they pass**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: exit 0, all seven new tests passing, and every pre-existing test still passing.

- [ ] **Step 5: Verify the test actually discriminates**

Temporarily change `ceilingZ` back to `region.maxZ` unconditionally, rebuild, re-run. Expected: `ClampedPointOnAnInteriorZSeamIsOwnedByThisServer` and `ClampedPointPastAnInteriorZSeamIsOwnedByThisServer` **fail**. Revert.

A test that passes against the defect is worth nothing, and this defect's whole history is a description that stopped matching the code.

- [ ] **Step 6: Point the server's function at the shared one, and correct both stale comments**

Replace the header comment at `ServerWorldManager.cpp:2378-2379` (the "mirror IsObjectInBorder exactly ... closed on Z" claim), the inline comment at `:2404` ("Z's upper bound is inclusive"), and the body at `:2392-2409`:

```cpp
// Clamps a handed-over object's position strictly inside this server's region.
//
// An object arriving from a neighbour can land exactly on, or a hair past, the shared
// edge: the sender decided the object had left its own region, but float rounding can
// leave the position on a coordinate this server's border test also rejects. Both
// servers then disown it. This nudge makes the receiver's test agree with the handoff
// that just happened.
//
// The bounds are NOT restated here. They were, and the copy went stale against the
// ownership unification - see NCL::Interaction::ClampIntoRegion, which is now the only
// definition and is tested directly against OwningServerFor at both 2- and 4-server
// partitions (tools/InteractionTests/RegionOwnershipTests.cpp).
//
// Both inputs come from GetRegionBounds(), the same partition GetObjectServer feeds to
// OwningServerFor - NOT from mServerBorderData. Reading the region from one source and
// the rule from another is how the two drifted apart in the first place.
Maths::Vector3 DistributedGameServer::ServerWorldManager::CalculateIncomingObjectOffsetPosition(
	const Maths::Vector3& position) const {
	const auto& regions = GetRegionBounds();

	float worldMinX = 0.0f, worldMaxX = 0.0f, worldMinZ = 0.0f, worldMaxZ = 0.0f;
	if (!GetWorldExtent(worldMinX, worldMaxX, worldMinZ, worldMaxZ)) {
		// No partition: OwningServerFor returns -1 for every point, so there is no
		// region to clamp into and no correction to make.
		return position;
	}

	for (const auto& region : regions) {
		if (region.serverId == mServerID) {
			return NCL::Interaction::ClampIntoRegion(region, worldMaxX, worldMaxZ, position);
		}
	}
	return position;
}
```

`GetWorldExtent(float&, float&, float&, float&)` is declared at `ServerWorldManager.h:207` and defined at `ServerWorldManager.cpp:2340`; it derives the world maxima from `GetRegionBounds()` itself and returns `false` only when the partition is empty. No new accessor is needed.

- [ ] **Step 7: Rebuild everything and re-run the full test suite**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: exit 0.

- [ ] **Step 8: Commit**

```bash
git add CSC8503CoreClasses/DistributedSystemCommonFiles/RegionOwnership.h \
        DistributedGameServer/ServerWorldManager.cpp \
        tools/InteractionTests/RegionOwnershipTests.cpp
git commit -m "fix(handoff): clamp arrivals under the ownership rule, not a stale copy of it"
```

**Note the clamp is still discarded at this point.** Task 1 makes it correct; Task 2 makes it act. Splitting them is what lets Gate A prove that Task 1 changed nothing.

---

### Task 2: Wire the clamp into the arrival path

Item 7, second half. This is a deliberate behaviour change at every lookahead, including 0.

**Files:**
- Modify: `DistributedGameServer/ServerWorldManager.cpp:2069-2078` (`ApplyIncomingObject`)

**Interfaces:**
- Consumes: `CalculateIncomingObjectOffsetPosition` from Task 1.
- Produces: `hoClamp` changes meaning — from "how often a clamp *would* have moved an object" to "how often one *did*". Task 8 records that in `docs/EVALUATION.md`.

- [ ] **Step 1: Record what hoClamp reads before the change**

From Task 0's analyses: the 2-server baseline is expected to show `hoClamp` 7–8 on server 0 with the halo on (`docs/EVALUATION.md` §7 item 7); the 4-server baseline has never been measured. Write both into the results document. This is the before-figure for the only counter this task moves.

- [ ] **Step 2: Apply the clamp instead of discarding it**

Replace the observation block at `ServerWorldManager.cpp:2069-2078`:

```cpp
	// Incoming handoffs that land outside the receiving region are now CORRECTED, not
	// merely counted. A reclaim lands outside this server's region by construction
	// (that is the whole point of a reclaim), so isReclaim is excluded or every reclaim
	// would be nudged and hoClamp would lose its value as a check on handoff targeting.
	//
	// hoClamp therefore counts arrivals that WERE moved. It counted arrivals that would
	// have been moved until 2026-08-26; figures either side of that are not comparable.
	if (!isReclaim && mServerBorderData != nullptr) {
		const Maths::Vector3 incoming = packet->lastFullState.position;
		const Maths::Vector3 clamped = CalculateIncomingObjectOffsetPosition(incoming);
		if (clamped.x != incoming.x || clamped.z != incoming.z) {
			++mHandoffsClamped;
			packet->lastFullState.position = clamped;
			// predictedPosition seeds CreateObjectFromArchetype for an object this
			// server has never seen, so leaving it unclamped would place a newly built
			// object outside the region while a promoted shadow landed inside it - two
			// arrival paths disagreeing, which is the class of defect this task closes.
			packet->lastFullState.predictedPosition = CalculateIncomingObjectOffsetPosition(
				packet->lastFullState.predictedPosition);
		}
	}
```

- [ ] **Step 3: Rebuild and confirm the unit suite is unaffected**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: exit 0. `ApplyIncomingObject` is not unit-tested — it needs a world — which is exactly why Task 1 put the geometry somewhere that is.

- [ ] **Step 4: Commit**

```bash
git add DistributedGameServer/ServerWorldManager.cpp
git commit -m "fix(handoff): nudge incoming objects into the receiving region"
```

---

### Task 3: Recognise a resend of a transfer already accepted

Item 15. The defect that produced 1,303 surviving duplicate objects on one repeat of three.

**Why the current guard cannot catch it.** `IsDuplicateHandoffArrival(isReclaim, objectPresent, networkActive, isHaloShadow)` returns true only when the object is present **and network-active**. A resend that arrives after this server has already handed the object onward finds it present in `mCreatedObjectPool` (pool entries are never erased — ids are never recycled) but **not** network-active. The guard returns false, the arrival falls through, and the object is re-installed here while the server it was handed to still owns it. Two owners, one object, integrated twice.

Object state cannot distinguish that from a genuine new handoff of an object returning here — both are "present, inactive". The distinguishing fact is on the *transfer*, not the object: a resend carries the identical `(senderServerID, mSenderTick)` as the transfer already accepted, because custody holds the packet verbatim (`transfer.packet = std::make_unique<StartSimulatingObjectPacket>(packet)`).

**Files:**
- Modify: `CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h`
- Modify: `tools/InteractionTests/HandoffCustodyTests.cpp`
- Modify: `DistributedGameServer/ServerWorldManager.h` (add `mAcceptedTransfers`)
- Modify: `DistributedGameServer/ServerWorldManager.cpp` (`ApplyIncomingObject`, after the existing duplicate guard at `:2030-2038`)

**Interfaces:**
- Produces: `NCL::Distributed::AcceptedTransfer { int senderServerID; long long senderTick; }` and `bool IsResendOfAcceptedTransfer(bool isReclaim, bool hasAcceptedRecord, const AcceptedTransfer& accepted, int arrivingSenderID, long long arrivingSenderTick)`.
- Produces: `ServerWorldManager::mAcceptedTransfers`, a `std::map<int, AcceptedTransfer>` keyed by object id. A `map`, not `unordered_map`, for the same reason `mScheduledReleases` is one — deterministic iteration order.

- [ ] **Step 1: Write the failing tests**

Append to `tools/InteractionTests/HandoffCustodyTests.cpp`:

```cpp
// --- IsResendOfAcceptedTransfer -------------------------------------------------
//
// THE REGRESSION TESTS FOR ITEM 15. IsDuplicateHandoffArrival tests OBJECT STATE, and
// object state cannot tell "a resend of a transfer I accepted and have since passed on"
// from "a genuine new handoff of an object coming back to me" - both are present-and-
// inactive. On three repeats of E4's rebalancing configuration, one ended holding 1,303
// MORE objects than the world contains (docs/superpowers/results/2026-08-24-B-e4-
// attribution.md). Duplication is a worse failure than loss for a physics simulation:
// the object exists twice, is integrated twice, and collides with itself.

TEST(ResendOfTheTransferAlreadyAcceptedIsRecognised) {
	// The exact defect: same sender, same tick, object long since handed onward.
	// Custody holds the packet verbatim, so a resend is byte-identical to the original.
	AcceptedTransfer accepted{2, 4096};
	CHECK(IsResendOfAcceptedTransfer(false, true, accepted, 2, 4096));
}

TEST(GenuineLaterTransferOfTheSameObjectIsNotAResend) {
	// The object left and came back. Same sender, LATER tick - it must be accepted, or
	// this guard converts a duplication defect into a loss defect.
	AcceptedTransfer accepted{2, 4096};
	CHECK(!IsResendOfAcceptedTransfer(false, true, accepted, 2, 5120));
}

TEST(TransferFromADifferentSenderAtTheSameTickIsNotAResend) {
	// Servers share no epoch (docs/EVALUATION.md section 5), so equal tick numbers from
	// two servers mean nothing. This is why the identity carries the sender id and not
	// the tick alone.
	AcceptedTransfer accepted{2, 4096};
	CHECK(!IsResendOfAcceptedTransfer(false, true, accepted, 3, 4096));
}

TEST(FirstTransferOfAnObjectIsNotAResend) {
	// No accepted record for this object id at all.
	AcceptedTransfer accepted{};
	CHECK(!IsResendOfAcceptedTransfer(false, false, accepted, 2, 4096));
}

TEST(ReclaimIsNeverTreatedAsAResend) {
	// A reclaim is the SENDER re-applying its own packet after the peer link died. It
	// carries the original (senderServerID, senderTick) by construction and would match
	// every time - and dropping it would strand the object nowhere, which is the loss
	// custody exists to prevent. Exempt for the same reason IsDuplicateHandoffArrival
	// exempts it.
	AcceptedTransfer accepted{2, 4096};
	CHECK(!IsResendOfAcceptedTransfer(true, true, accepted, 2, 4096));
}

TEST(AnEarlierTickThanTheAcceptedOneIsAlsoAResend) {
	// Reordered delivery: a resend can arrive after a later transfer was accepted. The
	// test is "not strictly newer", not "equal", or an out-of-order resend re-installs
	// the object exactly as before.
	AcceptedTransfer accepted{2, 5120};
	CHECK(IsResendOfAcceptedTransfer(false, true, accepted, 2, 4096));
}
```

- [ ] **Step 2: Run to verify they fail**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
```

Expected: **compile failure**, `'IsResendOfAcceptedTransfer': identifier not found`.

- [ ] **Step 3: Write the predicate**

Append to `HandoffCustody.h`, inside `namespace NCL::Distributed`, below `IsRedundantReclaim`:

```cpp
	// The identity of a transfer this server has already accepted for one object.
	// (senderServerID, senderTick) rather than a new sequence field: both are already on
	// StartSimulatingObjectPacket, so this costs no wire-format change, and custody holds
	// the packet verbatim so a resend reproduces both exactly.
	struct AcceptedTransfer {
		int       senderServerID = -1;
		long long senderTick = -1;
	};

	// True when an arriving transfer is a resend of one already accepted for this object.
	//
	// IsDuplicateHandoffArrival above cannot answer this. It tests object STATE, and a
	// resend that arrives after the object was handed ONWARD finds it present-but-
	// inactive, which is indistinguishable from a genuine new handoff of an object
	// returning here. So the arrival fell through and was re-installed while the server
	// it had been handed to still owned it: two owners, one object, integrated twice.
	//
	// The test is "not strictly newer than what we accepted", not "equal to it", because
	// delivery can reorder: a resend may arrive after a LATER transfer of the same object
	// was already accepted, and an equality test would let that one through.
	//
	// A reclaim is exempt. It is the sender re-applying its own packet after the peer
	// link died, so it carries the original identity by construction and would match
	// every time - and dropping it strands the object nowhere, the loss custody exists
	// to prevent. IsRedundantReclaim is what handles the reclaim case.
	inline bool IsResendOfAcceptedTransfer(bool isReclaim, bool hasAcceptedRecord,
		const AcceptedTransfer& accepted, int arrivingSenderID, long long arrivingSenderTick) {
		if (isReclaim) {
			return false;
		}
		if (!hasAcceptedRecord) {
			return false;
		}
		if (arrivingSenderID != accepted.senderServerID) {
			return false;
		}
		return arrivingSenderTick <= accepted.senderTick;
	}
```

- [ ] **Step 4: Run the tests to verify they pass**

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  DistributedPhysicsSystem.sln /t:Tools\InteractionTests /p:Configuration=Debug /p:Platform=x64
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: exit 0, six new tests passing.

- [ ] **Step 5: Verify the tests discriminate**

Temporarily weaken the predicate's last line to `return false;`, rebuild, re-run. Expected: `ResendOfTheTransferAlreadyAcceptedIsRecognised` and `AnEarlierTickThanTheAcceptedOneIsAlsoAResend` **fail**, and the four negative cases still pass. Revert.

- [ ] **Step 6: Record the accepted transfer, and consult the guard**

In `ServerWorldManager.h`, beside `mScheduledReleases`:

```cpp
			// The last transfer accepted for each object id, so a custody resend that
			// arrives after the object was handed ONWARD is recognisable as a resend
			// rather than re-installed as a second copy (item 15). std::map for
			// deterministic iteration, matching mScheduledReleases.
			//
			// Never pruned. An entry is 16 bytes against an object's several hundred,
			// ids are never recycled (NetworkIdSpace.h), and pruning would reintroduce
			// exactly the "no record, so accept it" case the guard exists to close.
			std::map<int, NCL::Distributed::AcceptedTransfer> mAcceptedTransfers;
```

In `ApplyIncomingObject`, immediately **after** the existing `IsDuplicateHandoffArrival` block (`:2030-2038`) and before the `IsRedundantReclaim` block:

```cpp
	// The object may be present-but-inactive because we handed it onward - see
	// IsResendOfAcceptedTransfer. Counted into mHandoffsDuplicate like the state-based
	// duplicate above: both are "a second arrival of one transfer", and hoDup is the
	// axis they are reported on so neither enters the I5 parity sum.
	{
		const auto acceptedEntry = mAcceptedTransfers.find(packet->objectID);
		const bool hasAccepted = acceptedEntry != mAcceptedTransfers.end();
		const NCL::Distributed::AcceptedTransfer accepted =
			hasAccepted ? acceptedEntry->second : NCL::Distributed::AcceptedTransfer{};
		if (NCL::Distributed::IsResendOfAcceptedTransfer(isReclaim, hasAccepted, accepted,
			packet->senderServerID, packet->mSenderTick)) {
			++mHandoffsDuplicate;
			std::cout << "Resend of an already-accepted transfer for object "
				<< packet->objectID << " (sender " << packet->senderServerID
				<< ", tick " << packet->mSenderTick
				<< ") - acked without re-installing.\n";
			return true;
		}
	}
```

Then record acceptance on the successful-install path. Find it first:

```bash
sed -n '2005,2200p' DistributedGameServer/ServerWorldManager.cpp
```

Place this on the **single** path that has installed the object and is about to return success — not on each early return:

```cpp
	// Recorded on acceptance, not on arrival: a transfer that failed to install must not
	// suppress its own resend, which is the whole point of the resend.
	if (!isReclaim) {
		mAcceptedTransfers[packet->objectID] =
			NCL::Distributed::AcceptedTransfer{packet->senderServerID, packet->mSenderTick};
	}
```

- [ ] **Step 7: Rebuild all roles and re-run the suite**

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
.\tools\InteractionTests\Debug\InteractionTests.exe
```

Expected: exit 0.

- [ ] **Step 8: Commit**

```bash
git add CSC8503CoreClasses/DistributedSystemCommonFiles/HandoffCustody.h \
        DistributedGameServer/ServerWorldManager.h \
        DistributedGameServer/ServerWorldManager.cpp \
        tools/InteractionTests/HandoffCustodyTests.cpp
git commit -m "fix(handoff): recognise a custody resend by transfer identity, not object state"
```

---

### Task 4: Gate A — bound what Tasks 1 and 3 could have changed

Run this **after Task 3 and before Task 5**. Both Task 1 and Task 3 are intended to be no-ops on a healthy-path run: Task 1 only changes a value that is still discarded, and Task 3 only fires when a resend arrives for an object already handed onward, which does not happen when `hoResent` is 0.

**Files:** none modified. Produces `runs/exp-dA-gate2/`, `runs/exp-dA-gate4/`.

**Interfaces:**
- Consumes: `runs/exp-d0-base2`, `runs/exp-d0-base4` from Task 0.
- Produces: the verdict Task 5 requires before it changes the default.

- [ ] **Step 1: Take three post-change repeats of each baseline configuration**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name dA-gate2 -Sweep servers -Values "2" -Repeats 3 `
  -Objects 400 -Ticks 1800 -Seed 42 -Workload uniform `
  -HaloWidth 8 -HaloReliable -HandoffLookahead 0 -DrainSeconds 30

powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name dA-gate4 -Sweep servers -Values "4" -Repeats 3 `
  -Objects 400 -Ticks 1800 -Seed 42 -Workload uniform `
  -HaloWidth 8 -HaloReliable -HandoffLookahead 0 -DrainSeconds 30
```

- [ ] **Step 2: Check validity before comparing**

```powershell
python tools\analyse.py runs\exp-dA-gate2
python tools\analyse.py runs\exp-dA-gate4
```

A gate comparison between two invalid runs proves nothing. `hoCustody` must be 0 on both sides; raise `-DrainSeconds` and re-take if not.

- [ ] **Step 3: Compare against the step-0 baselines**

```powershell
python tools\gate-compare.py runs\exp-d0-base2 -- runs\exp-dA-gate2
python tools\gate-compare.py runs\exp-d0-base4 -- runs\exp-dA-gate4
```

Expected: **GATE PASSED** on both — 20 stable fields unchanged, 2 conserved totals unchanged.

- [ ] **Step 4: If the 4-server gate fails, diagnose before proceeding**

The 4-server baseline is new, so its own repeat-to-repeat spread is unmeasured. A failure here may be Task 1 or 3 misbehaving, or it may be that a field stable at 2 servers is not stable at 4 — `gate-compare.py`'s `STABLE` set was derived from 2-server data in Phase A and has never been checked at 4.

Distinguish them: re-run the **baseline** configuration three more times at the step-0 commit and gate-compare baseline against baseline. If that fails too, the field is not stable at 4 servers and belongs in a range-checked set, exactly as Phase A reclassified `haloLate` (ruling P11). Record the reclassification and name its blind spot.

- [ ] **Step 5: Commit the gate record**

```bash
git add docs/superpowers/results/2026-08-26-D-ownership.md
git commit -m "docs(D): gate A - tasks 1 and 3 are no-ops on the healthy path"
```

---

### Task 5: Make scheduled release the default

Item 2. The change with the largest blast radius in the phase, taken last of the code changes so Gate A has already bounded the other two.

**Files:**
- Modify: `DistributedGameServer/ServerStarter.cpp:113`
- Produces: `runs/exp-d5-look<L>-s2/`, `runs/exp-d5-look<L>-s4/`, `runs/exp-d5-explicit0/`

**Interfaces:**
- Consumes: Gate A's verdict (Task 4) — do not start this task until Gate A has passed.

- [ ] **Step 1: Assert the halo bound on the candidate lookaheads (ruling D5)**

A lookahead of L ticks means the sender simulates the object for L ticks *after* it has left the sender's region. The receiver only sees it during that window if it is inside the halo band:

```
v_max * L * dt <= halo_width
```

At the baseline configuration (`--halo-width 8`, dt = 1/120 s), compute `v_max` from the workload and solve for the largest admissible L. Write the number into the results document **before** running the sweep, so the sweep's range is justified rather than convenient. If the bound admits fewer than the sweep's largest value, shorten the sweep — do not run points the design already excludes.

This is Phase C §4.5's lesson in the opposite direction: a sweep that never samples the predicted point can only return all-pass or all-fail.

- [ ] **Step 2: Sweep the lookahead at 2 servers**

```powershell
foreach ($L in 0,2,4,8,16) {
  powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name "d5-look$L-s2" -Sweep servers -Values "2" -Repeats 3 `
    -Objects 400 -Ticks 1800 -Seed 42 -Workload uniform `
    -HaloWidth 8 -HaloReliable -HandoffLookahead $L -DrainSeconds 30
}
```

- [ ] **Step 3: Sweep the lookahead at 4 servers**

```powershell
foreach ($L in 0,2,4,8,16) {
  powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
    -Name "d5-look$L-s4" -Sweep servers -Values "4" -Repeats 3 `
    -Objects 400 -Ticks 1800 -Seed 42 -Workload uniform `
    -HaloWidth 8 -HaloReliable -HandoffLookahead $L -DrainSeconds 30
}
```

- [ ] **Step 4: Pick the default from the data (ruling D4)**

```powershell
foreach ($L in 0,2,4,8,16) {
  python tools\analyse.py "runs\exp-d5-look$L-s2"
  python tools\analyse.py "runs\exp-d5-look$L-s4"
}
```

The default is the **smallest** L with `ownership_gap_ticks = 0`, `ownership_double_ticks = 0` and `hoLate = 0` at **both** server counts, and satisfying Step 1's bound. Tabulate every point, including the ones that fail — a sweep reported only at its winner cannot be checked.

If no L satisfies all three, **stop and report**. That outcome means the ownership gap is not closable by scheduling alone at this configuration, which is a finding worth more than a default, and it is not this plan's job to invent a mechanism for it.

- [ ] **Step 5: Change the default**

`ServerStarter.cpp:113`, substituting the L chosen in Step 4:

```cpp
	// Default chosen by the Task 5 sweep, not by argument: the smallest lookahead at
	// which no tick has an object owned by nobody or by two servers, at both 2 and 4
	// servers, and small enough that an object released L ticks after leaving the
	// sender's region is still inside the receiver's halo band (v_max * L * dt <=
	// halo_width). 0 - release on send - was the default until 2026-08-26 and left
	// nobody owning the object for one network round trip: 1,397 of 1,800 ticks on a
	// uniform run. See docs/superpowers/results/2026-08-26-D-ownership.md.
	worldManager->SetHandoffLookaheadTicks(config.GetInt("--handoff-lookahead", <L>));
```

- [ ] **Step 6: Confirm the old default is still reachable explicitly — §5.6's gate**

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-deploy.ps1 -Config Release
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name d5-explicit0 -Sweep servers -Values "2" -Repeats 3 `
  -Objects 400 -Ticks 1800 -Seed 42 -Workload uniform `
  -HaloWidth 8 -HaloReliable -HandoffLookahead 0 -DrainSeconds 30
python tools\gate-compare.py runs\exp-dA-gate2 -- runs\exp-d5-explicit0
```

Expected: **GATE PASSED**. This is §5.6 in its stated form — an explicit 0 must still behave as 0 did. Compared against Gate A rather than the step-0 baseline, per ruling D1, because Task 2 legitimately moved `hoClamp` in between.

A failure on a field Task 2 moves (`hoClamp`, and the conserved totals if the clamp changed where objects land) must be recorded with the field named — not waved through. A failure on any *other* field is a defect in the default change.

- [ ] **Step 7: Commit**

```bash
git add DistributedGameServer/ServerStarter.cpp
git commit -m "feat(handoff): schedule release by default, so ownership changes atomically"
```

---

### Task 6: Re-run the conservation-sensitive experiments

Spec §5.7, extended with the 4-server axis (ruling D3) and with the repeat count item 15 needs.

**Files:** none modified. Produces `runs/exp-d6-E4/`, `-E4-s4/`, `-E7/`, `-E1/`, `-E2/`.

- [ ] **Step 1: Re-run E4 with enough repeats to see item 15**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name d6-E4 -Sweep rebalanceInterval -Values "0,400" -Repeats 6 `
  -Servers 2 -Objects 4000 -Ticks 7200 -Seed 42 -Workload cluster `
  -HandoffLookahead 300 -DrainSeconds 120
```

**Six repeats, not three.** Item 15 appeared in 1 of 3 — at three repeats a clean result is indistinguishable from a miss. Examine `hoDup` and `hoResent` per repeat, not only `conservation_delta`: a duplicate that has not survived to exit still means the path fired.

- [ ] **Step 2: Re-run E4 at 4 servers, and expect the topology discontinuity**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name d6-E4-s4 -Sweep rebalanceInterval -Values "0,400" -Repeats 6 `
  -Servers 4 -Objects 4000 -Ticks 7200 -Seed 42 -Workload cluster `
  -HandoffLookahead 300 -DrainSeconds 120
```

**This has never been run.** Spec §5.4: the initial partition is a 2×2 grid, but the repartition path emits 1-D X slices only (`SystemManager.cpp:386-389`), so the first rebalance reshapes the whole partition rather than moving one border. Every region changes at once, so every object is potentially in flight simultaneously — the condition item 15 needs, maximised.

Report this run as **measuring a whole-partition reshape**, not incremental border movement. Do not fold it into E4's balancing claim; it is a correctness run that happens to use E4's configuration.

- [ ] **Step 3: Re-run E7 at both server counts**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name d6-E7 -Sweep servers -Values "2,4" -Repeats 3 `
  -Objects 8000 -Ticks 1800 -Seed 42 -Workload uniform `
  -HaloWidth 8 -HaloReliable -DrainSeconds 60
```

- [ ] **Step 4: Re-run E1 and E2 as regressions**

```powershell
powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name d6-E1 -Sweep servers -Values "1,2,4" -Repeats 3 `
  -Objects 1600 -Ticks 1800 -Seed 42 -Workload uniform -HaloWidth 0

powershell -ExecutionPolicy Bypass -File tools\run-experiments.ps1 `
  -Name d6-E2 -Sweep haloWidth -Values "0,8" -Repeats 3 `
  -Servers 2 -Objects 100 -Ticks 1800 -Seed 42 -Workload headon -HaloReliable
```

E1 measures state and E2 measures events, so neither should move. Showing that is the point — §5.7 lists them as regressions precisely because "should be untouched" is not evidence.

- [ ] **Step 5: Analyse everything**

```powershell
python tools\analyse.py runs\exp-d6-E4
python tools\analyse.py runs\exp-d6-E4-s4
python tools\analyse.py runs\exp-d6-E7
python tools\analyse.py runs\exp-d6-E1
python tools\analyse.py runs\exp-d6-E2
```

Expected on every experiment: `conservation_delta = 0`, `ownership_gap_ticks = 0`, `ownership_double_ticks = 0`.

`hoDup > 0` with `conservation_delta = 0` means the guard fired and did its job — report it as the guard working, not as a failure. `hoDup > 0` with a **positive** `conservation_delta` means a duplicate survived and item 15 is **not** closed.

- [ ] **Step 6: Commit the results**

```bash
git add docs/superpowers/results/2026-08-26-D-ownership.md
git commit -m "docs(D): re-measure E1, E2, E4 and E7 at 2 and 4 servers"
```

---

### Task 7: Write the results document

**Files:** Create `docs/superpowers/results/2026-08-26-D-ownership.md` (written incrementally from Task 0; this task completes it).

- [ ] **Step 1: Write it, covering every section below**

1. **Outcome per item** — 2, 7, 15: closed or not, each with the number that says so.
2. **The step-0 baselines**, including the first 4-server conservation figures ever taken and anything they showed that 2 servers could not.
3. **Gate A**, and any field reclassified out of `STABLE` at 4 servers, with its blind spot named.
4. **The lookahead sweep**, every point tabulated including failures, with the derivation of the chosen default and the halo bound from ruling D5.
5. **The §5.6 explicit-zero gate**, and which fields Task 2 legitimately moved.
6. **The re-runs**, with the 4-server E4 reported as a whole-partition reshape.
7. **Deferred, explicitly** — in the pattern of Phase C §4.8's deferred list. Two are known now: the repartition path still emits 1-D slices (§5.4 is recorded, not fixed); and no latency was injected in this phase, so the ownership fix is measured at `T_L = 0` even though §5.1's whole argument for ordering D after C was that injected latency is what makes the gap proportional to `T_L`. Say plainly whether that argument was honoured or deferred.

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/results/2026-08-26-D-ownership.md
git commit -m "docs(D): phase D results"
```

---

### Task 8: Correct the documentation, and give item 15 a spec section

Spec §5.5's table, plus the spec gap this plan opened with.

**Files:** `docs/superpowers/specs/2026-08-23-backlog-completion-design.md`, `docs/EVALUATION.md`, `docs/SPATIAL-PARTITIONING.md`, `CLAUDE.md`.

- [ ] **Step 1: Verify every claim is still stale before editing it**

```bash
grep -n "mirror .IsObjectInBorder\|upper bound is inclusive" DistributedGameServer/ServerWorldManager.cpp
grep -n "CalculateIncomingObjectOffsetedPosition" docs/SPATIAL-PARTITIONING.md
grep -n "the non-default case" docs/EVALUATION.md
```

Phase A's Task 8 did this and it is not ceremony: a claim corrected in passing by an earlier task and then "corrected" again reads as two defects where there was one.

- [ ] **Step 2: Add spec §5.8 — item 15**

Into `docs/superpowers/specs/2026-08-23-backlog-completion-design.md`, after §5.7: the mechanism (a state-based guard cannot see a resend arriving after onward handoff), the fix (transfer identity on `(senderServerID, mSenderTick)`, no wire change), why a reclaim is exempt, and why the accepted-transfer map is never pruned. Correct §5.2 to say the phase carries three items, not two, and §5.7 to carry the 4-server axis and E4's six repeats.

- [ ] **Step 3: Correct `docs/SPATIAL-PARTITIONING.md:52-54`**

It names `CalculateIncomingObjectOffsetedPosition` (no such name — there is no "ed"), cites `ServerWorldManager.cpp:296-314` (the function is now at `:2392`), and describes it as having "most of its branches currently commented out, leaving only a Z-axis floor adjustment active" — the pre-rewrite state. Replace with what it does after Tasks 1 and 2, and add §5.4's partition-topology discontinuity: the initial partition is a 2-D grid, repartitioning emits 1-D X slices, and nothing currently documents that they differ.

- [ ] **Step 4: Correct `docs/EVALUATION.md`**

- §7 item 7: closes. Say what the function now does, and that `hoClamp` changed meaning on 2026-08-26 (would-have-moved → did-move), so figures either side are not comparable.
- §7 item 2: closes or narrows, per Task 5's outcome.
- §7 item 15: closes or stays open, per Task 6 Step 5.
- §6: the conditional ownership guarantee — "conditional on `--handoff-lookahead > 0`, the non-default case" — changes meaning when that becomes the default.
- §5: gains the 4-server correctness coverage this phase established, and keeps its statement that no 4-server *timing* figure is claimed (ruling D3).

- [ ] **Step 5: Correct `CLAUDE.md`**

Two bullets in the verified-state block: the offset-function bullet (currently says the body is wrong — after Task 1 it is right, after Task 2 it is wired) and the ownership-gap bullet (currently says the gap "is the default behaviour, not an edge case").

- [ ] **Step 6: Verify no stale reference survives**

```bash
grep -rn "CalculateIncomingObjectOffsetedPosition" docs/ CLAUDE.md
grep -rn "the non-default case" docs/EVALUATION.md
```

Expected: no matches.

- [ ] **Step 7: Commit**

```bash
git add docs/ CLAUDE.md
git commit -m "docs(D): correct the ownership and clamp claims across four documents"
```

---

## Phase D completion checklist

- [ ] Items 2, 7 and 15 each have a stated outcome backed by a number
- [ ] `InteractionTests` exits 0, and every new test was shown to fail against the defect it pins
- [ ] Gate A passed at 2 **and** 4 servers, or a reclassified field is named with its blind spot
- [ ] The §5.6 explicit-`--handoff-lookahead 0` gate passed, with any field Task 2 moved named
- [ ] The new default lookahead is derived from the sweep, and every swept point is tabulated
- [ ] E1, E2, E4 and E7 re-run; E4 at 6 repeats; E4 and E7 at 4 servers
- [ ] Spec §5.8 written; §5.5's four documents corrected; the stale-reference greps come back empty
- [ ] The deferred list says plainly whether the ownership fix was measured under injected latency
