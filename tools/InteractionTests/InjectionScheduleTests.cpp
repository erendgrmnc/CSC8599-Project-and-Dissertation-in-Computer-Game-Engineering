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
