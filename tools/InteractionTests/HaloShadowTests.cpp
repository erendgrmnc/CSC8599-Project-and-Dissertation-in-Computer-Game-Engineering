#include "TestHarness.h"

#include "GameWorld.h"
#include "GameObject.h"
#include "PhysicsSystem.h"
#include "PhysicsObject.h"
#include "AABBVolume.h"
#include "Profiler.h"

using namespace NCL;
using namespace NCL::CSC8503;

// A halo shadow is a read-only copy of an object owned by a NEIGHBOURING server,
// held only so that objects either side of a region border can collide.
//
// Invariant I7 (single simulator): exactly one server integrates any given object.
// A shadow that gets integrated makes this server a second owner, the two copies
// diverge within a tick, and nothing anywhere reports it - which is why the property
// is pinned here rather than left to a system run to notice.
namespace {
	constexpr float TICK_DT = 0.05f;

	GameObject* MakeDynamicCube(const Vector3& position) {
		GameObject* cube = new GameObject(NoSpecialFeatures, "cube");
		cube->SetBoundingVolume((CollisionVolume*)new AABBVolume(Vector3(1, 1, 1)));
		cube->GetTransform().SetScale(Vector3(2, 2, 2)).SetPosition(position);
		cube->SetPhysicsObject(new PhysicsObject(&cube->GetTransform(), cube->GetBoundingVolume()));
		cube->GetPhysicsObject()->SetInverseMass(1.0f);
		cube->GetPhysicsObject()->InitCubeInertia();
		cube->SetActive(true);
		return cube;
	}

	// What the halo will build: an ordinary object in every respect except that it is
	// flagged as belonging to someone else. Deliberately still active and still
	// physics-enabled - a shadow that had physics switched off would be skipped by the
	// broadphase and could not collide, which is the entire reason it exists.
	GameObject* MakeShadowCube(const Vector3& position) {
		GameObject* cube = MakeDynamicCube(position);
		cube->SetIsHaloShadow(true);
		return cube;
	}
}

TEST(HaloShadowIsNotIntegrated) {
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	GameObject* owned = MakeDynamicCube(Vector3(0, 50, 0));
	GameObject* shadow = MakeShadowCube(Vector3(40, 50, 0));
	world.AddGameObject(owned);
	world.AddGameObject(shadow);

	const Vector3 shadowStart = shadow->GetTransform().GetPosition();

	physics.Update(TICK_DT);

	// One of the two was integrated, not both.
	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);

	// The owned object fell; the shadow did not move at all. Its owner is responsible
	// for its motion and will send the result.
	CHECK(owned->GetTransform().GetPosition().y < shadowStart.y);
	CHECK_EQ(shadow->GetTransform().GetPosition().y, shadowStart.y);
	CHECK_EQ(shadow->GetTransform().GetPosition().x, shadowStart.x);

	world.ClearAndErase();
}

TEST(HaloShadowGainsNoVelocityFromGravity) {
	// Position is the visible symptom, but velocity is what actually corrupts the
	// next contact: relative velocity feeds the impulse. A shadow that accumulates
	// gravity would resolve border contacts against a body that is falling on one
	// server and stationary on the other, and the two sides would compute different
	// impulses (invariant I8).
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	GameObject* shadow = MakeShadowCube(Vector3(0, 50, 0));
	world.AddGameObject(shadow);

	for (int tick = 0; tick < 10; ++tick) {
		physics.Update(TICK_DT);
	}

	const Vector3 velocity = shadow->GetPhysicsObject()->GetLinearVelocity();
	CHECK_EQ(velocity.x, 0.0f);
	CHECK_EQ(velocity.y, 0.0f);
	CHECK_EQ(velocity.z, 0.0f);
	CHECK_EQ(Profiler::GetIntegratedObjects(), 0);

	world.ClearAndErase();
}

TEST(HaloShadowStillFormsBroadphasePairs) {
	// The point of a shadow. If it were excluded from the broadphase the way a
	// deactivated object is, it would be an expensive way to store nothing.
	//
	// Two overlapping cubes, one owned and one a shadow: the contact must be found
	// and resolved, which shows up as a non-zero contact count.
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(false);

	GameObject* owned = MakeDynamicCube(Vector3(0, 0, 0));
	GameObject* shadow = MakeShadowCube(Vector3(1.0f, 0, 0));
	world.AddGameObject(owned);
	world.AddGameObject(shadow);

	const long long before = Profiler::GetContactsResolvedTotal();
	physics.Update(TICK_DT);

	// The cumulative total, not the last substep's count. Two overlapping bodies at
	// rest are pushed apart by penetration resolution over the substeps of a single
	// Update, so by the final substep they are no longer in contact and the per-substep
	// figure is legitimately 0.
	//
	// This test used to read the per-substep count and pass anyway, because the old
	// broadphase paired every object with ITSELF (its inner loop started at j = i), so
	// the count never fell to zero. Those self-pairs were resolved as contacts and
	// inflated every contact measurement by one per object per substep.
	CHECK(Profiler::GetContactsResolvedTotal() > before);

	world.ClearAndErase();
}

TEST(ContactWithAShadowStillMovesTheOwnedObject) {
	// A shadow must push back. Resolving the contact but leaving the owned object
	// untouched would be the same failure as having no shadow at all, only slower.
	//
	// The owned object is given velocity TOWARDS the shadow: ImpulseResolveCollision
	// responds to approaching relative velocity, so two bodies merely overlapping at
	// rest generate no impulse at all - only positional separation.
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(false);

	GameObject* owned = MakeDynamicCube(Vector3(0, 0, 0));
	GameObject* shadow = MakeShadowCube(Vector3(3.0f, 0, 0));
	owned->GetPhysicsObject()->SetLinearVelocity(Vector3(20.0f, 0, 0));
	world.AddGameObject(owned);
	world.AddGameObject(shadow);

	for (int tick = 0; tick < 5; ++tick) {
		physics.Update(TICK_DT);
	}

	// Was moving at +20 straight at the shadow; the contact must have taken that away.
	CHECK(owned->GetPhysicsObject()->GetLinearVelocity().x < 20.0f);

	world.ClearAndErase();
}

TEST(ContactCorruptsShadowStateAndSoItMustBeReimposed) {
	// The requirement B4 has to satisfy, pinned as a test so it cannot be forgotten.
	//
	// Skipping the integrator is NOT sufficient to make a shadow read-only. Contact
	// resolution reaches around the integrator and writes to both bodies directly:
	//
	//   - ImpulseResolveCollision writes linear and angular VELOCITY to both,
	//   - SeperateObjects writes POSITION to both, to resolve penetration.
	//
	// So a shadow is moved and accelerated by every contact it takes part in, and
	// drifts away from the copy its owner is actually simulating. The two servers then
	// disagree about where the object is and how fast it is going, and compute
	// different impulses from it - invariant I8, broken silently.
	//
	// This is not a bug in the physics system: on one server, moving both bodies is
	// exactly right. It is the reason a shadow's authoritative state has to be
	// re-imposed at the top of EVERY tick, not only when an update arrives.
	//
	// The test asserts the corruption EXISTS. When B4 lands it should be inverted to
	// assert the state is restored.
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(false);

	GameObject* owned = MakeDynamicCube(Vector3(0, 0, 0));
	GameObject* shadow = MakeShadowCube(Vector3(1.0f, 0, 0));   // deeply overlapping
	owned->GetPhysicsObject()->SetLinearVelocity(Vector3(20.0f, 0, 0));
	world.AddGameObject(owned);
	world.AddGameObject(shadow);

	const float startX = shadow->GetTransform().GetPosition().x;
	for (int tick = 0; tick < 5; ++tick) {
		physics.Update(TICK_DT);
	}

	// Displaced by penetration resolution...
	CHECK(shadow->GetTransform().GetPosition().x != startX);
	// ...and accelerated by the impulse, despite never being integrated.
	CHECK(shadow->GetPhysicsObject()->GetLinearVelocity().x != 0.0f);
	// The integrator itself still never touched it.
	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);

	world.ClearAndErase();
}

TEST(ClearingTheShadowFlagRestoresOrdinarySimulation) {
	// B6 turns a handoff into a promotion: the receiving server already holds the
	// object as a shadow and simply takes ownership of it. That promotion is exactly
	// this flag being cleared, so it must be enough on its own.
	GameWorld world;
	PhysicsSystem physics(world);
	physics.UseGravity(true);

	GameObject* object = MakeShadowCube(Vector3(0, 50, 0));
	world.AddGameObject(object);

	physics.Update(TICK_DT);
	CHECK_EQ(Profiler::GetIntegratedObjects(), 0);

	object->SetIsHaloShadow(false);
	physics.Update(TICK_DT);
	CHECK_EQ(Profiler::GetIntegratedObjects(), 1);
	CHECK(object->GetPhysicsObject()->GetLinearVelocity().y < 0.0f);

	world.ClearAndErase();
}

TEST(ObjectsDefaultToNotBeingShadows) {
	// The flag is added to a class every object in the engine goes through. A default
	// of true, or an uninitialised member, would stop the world simulating with no
	// obvious cause.
	GameObject plain(NoSpecialFeatures, "plain");
	CHECK(!plain.IsHaloShadow());
}
