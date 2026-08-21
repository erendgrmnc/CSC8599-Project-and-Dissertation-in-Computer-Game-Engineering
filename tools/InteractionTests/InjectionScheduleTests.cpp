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
