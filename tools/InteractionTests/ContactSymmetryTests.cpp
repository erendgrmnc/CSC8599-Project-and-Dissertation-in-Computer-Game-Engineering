#include "TestHarness.h"

#include "GameWorld.h"
#include "GameObject.h"
#include "PhysicsSystem.h"
#include "PhysicsObject.h"
#include "AABBVolume.h"
#include "CollisionDetection.h"
#include "Profiler.h"

#include <cmath>

using namespace NCL;
using namespace NCL::CSC8503;

// Invariant I8, symmetric contact.
//
// A contact between an object owned by server A and one owned by server B is
// resolved on BOTH servers - each keeps the half that applies to the object it owns
// and discards the other. That is only correct if both sides compute the SAME
// impulse, and floating point does not commute: ImpulseResolveCollision(A, B) and
// ImpulseResolveCollision(B, A) do the same physics in a different operand order and
// give bit-different answers.
//
// So the pair must be oriented identically on both servers. It cannot be oriented by
// world ID: GameWorld::AddGameObject hands those out as a creation counter, and the
// two servers create the pair in opposite orders - the owned object at pre-seed, the
// neighbour's shadow later. GetContactOrderID exists for this, and these tests are
// what stop it quietly reverting to the world ID.
namespace {
	constexpr float TICK_DT = 0.05f;

	// Half-extent 0.5, so a pair centred 0.9 apart is overlapping and in contact.
	GameObject* MakeCube(const Vector3& position, const Vector3& velocity, int contactOrderID) {
		GameObject* cube = new GameObject(NoSpecialFeatures, "cube");
		cube->SetBoundingVolume((CollisionVolume*)new AABBVolume(Vector3(0.5f, 0.5f, 0.5f)));
		cube->GetTransform().SetScale(Vector3(1, 1, 1)).SetPosition(position);
		cube->SetPhysicsObject(new PhysicsObject(&cube->GetTransform(), cube->GetBoundingVolume()));
		cube->GetPhysicsObject()->SetInverseMass(1.0f);
		cube->GetPhysicsObject()->InitCubeInertia();
		cube->GetPhysicsObject()->SetLinearVelocity(velocity);
		cube->SetActive(true);
		cube->SetContactOrderID(contactOrderID);
		return cube;
	}
}

TEST(ContactOrderIdDefaultsToUnsetAndFallsBackToWorldId) {
	// Static geometry has no global identity. Such objects must still order
	// deterministically against each other, and must never interleave with the
	// networked ones, or the comparator stops being a strict weak ordering.
	GameObject withGlobal(NoSpecialFeatures, "net");
	GameObject withoutGlobal(NoSpecialFeatures, "floor");
	withGlobal.SetContactOrderID(7);
	withGlobal.SetWorldID(99);
	withoutGlobal.SetWorldID(0);

	const auto keyGlobal = CollisionDetection::CollisionInfo::OrderKey(&withGlobal);
	const auto keyLocal = CollisionDetection::CollisionInfo::OrderKey(&withoutGlobal);

	CHECK_EQ(keyGlobal.first, 0);
	CHECK_EQ(keyGlobal.second, 7);
	CHECK_EQ(keyLocal.first, 1);
	CHECK_EQ(keyLocal.second, 0);
	// Despite the much larger world ID, the networked object sorts first.
	CHECK(keyGlobal < keyLocal);
}

TEST(ContactOrderIsIndependentOfCreationOrder) {
	// The whole point. Two worlds build the same two objects in opposite orders, so
	// their world IDs come out swapped; the canonical order must not.
	GameWorld worldA;
	GameObject* a1 = MakeCube(Vector3(0, 0, 0), Vector3(0, 0, 0), 100);
	GameObject* a2 = MakeCube(Vector3(1, 0, 0), Vector3(0, 0, 0), 200);
	worldA.AddGameObject(a1);
	worldA.AddGameObject(a2);

	GameWorld worldB;
	GameObject* b2 = MakeCube(Vector3(1, 0, 0), Vector3(0, 0, 0), 200);
	GameObject* b1 = MakeCube(Vector3(0, 0, 0), Vector3(0, 0, 0), 100);
	worldB.AddGameObject(b2);   // created FIRST here
	worldB.AddGameObject(b1);

	// World IDs disagree about which came first...
	CHECK(a1->GetWorldID() < a2->GetWorldID());
	CHECK(b2->GetWorldID() < b1->GetWorldID());

	// ...but the contact order does not.
	const bool aFirstInA = CollisionDetection::CollisionInfo::OrderKey(a1)
		< CollisionDetection::CollisionInfo::OrderKey(a2);
	const bool aFirstInB = CollisionDetection::CollisionInfo::OrderKey(b1)
		< CollisionDetection::CollisionInfo::OrderKey(b2);
	CHECK(aFirstInA);
	CHECK(aFirstInB);

	worldA.ClearAndErase();
	worldB.ClearAndErase();
}

TEST(CrossBorderContactIsSymmetricBetweenTheTwoServers) {
	// The invariant itself, simulated end to end.
	//
	// Server A owns the left object and shadows the right one; server B is the mirror
	// image. Each resolves the same contact independently. The momentum each side
	// gives ITS OWN object must be equal and opposite, or the pair gains or loses
	// momentum at the border and the two servers disagree about what happened.
	//
	// The objects are created in opposite orders in the two worlds, exactly as they
	// are in the real system - the owned one is built at pre-seed and the shadow when
	// its first halo update arrives.
	const Vector3 leftStart(0, 0, 0);
	const Vector3 rightStart(0.9f, 0, 0);
	const Vector3 leftVelocity(20, 0, 0);
	const Vector3 rightVelocity(-20, 0, 0);
	const int leftID = 100;
	const int rightID = 200;

	// --- server A: owns left, shadows right ---
	GameWorld worldA;
	PhysicsSystem physicsA(worldA);
	physicsA.UseGravity(false);
	GameObject* ownedLeft = MakeCube(leftStart, leftVelocity, leftID);
	GameObject* shadowRight = MakeCube(rightStart, rightVelocity, rightID);
	shadowRight->SetIsHaloShadow(true);
	worldA.AddGameObject(ownedLeft);
	worldA.AddGameObject(shadowRight);
	physicsA.Update(TICK_DT);
	const Vector3 leftAfter = ownedLeft->GetPhysicsObject()->GetLinearVelocity();

	// --- server B: owns right, shadows left. Created in the OTHER order. ---
	GameWorld worldB;
	PhysicsSystem physicsB(worldB);
	physicsB.UseGravity(false);
	GameObject* ownedRight = MakeCube(rightStart, rightVelocity, rightID);
	GameObject* shadowLeft = MakeCube(leftStart, leftVelocity, leftID);
	shadowLeft->SetIsHaloShadow(true);
	worldB.AddGameObject(ownedRight);
	worldB.AddGameObject(shadowLeft);
	physicsB.Update(TICK_DT);
	const Vector3 rightAfter = ownedRight->GetPhysicsObject()->GetLinearVelocity();

	// Both must actually have found the contact; a test where neither collides would
	// pass the symmetry check trivially.
	CHECK(leftAfter.x != leftVelocity.x);
	CHECK(rightAfter.x != rightVelocity.x);

	// Equal masses, so equal and opposite velocity changes.
	const float leftDelta = leftAfter.x - leftVelocity.x;
	const float rightDelta = rightAfter.x - rightVelocity.x;
	CHECK_NEAR(leftDelta, -rightDelta, 1e-4f);

	worldA.ClearAndErase();
	worldB.ClearAndErase();
}

TEST(CrossBorderContactAgreesOnWhereEachObjectEndsUp) {
	// Stronger than the impulse check: each server must also agree about the OTHER
	// object's resulting velocity, because that is what it will be sent next tick. If
	// the two disagreed, every border contact would inject a small discrepancy that
	// the halo would then dutifully replicate back and forth.
	const Vector3 leftStart(0, 0, 0);
	const Vector3 rightStart(0.9f, 0, 0);
	const Vector3 leftVelocity(20, 0, 0);
	const Vector3 rightVelocity(-20, 0, 0);

	GameWorld worldA;
	PhysicsSystem physicsA(worldA);
	physicsA.UseGravity(false);
	GameObject* aLeft = MakeCube(leftStart, leftVelocity, 100);
	GameObject* aRight = MakeCube(rightStart, rightVelocity, 200);
	aRight->SetIsHaloShadow(true);
	worldA.AddGameObject(aLeft);
	worldA.AddGameObject(aRight);
	physicsA.Update(TICK_DT);

	GameWorld worldB;
	PhysicsSystem physicsB(worldB);
	physicsB.UseGravity(false);
	GameObject* bRight = MakeCube(rightStart, rightVelocity, 200);
	GameObject* bLeft = MakeCube(leftStart, leftVelocity, 100);
	bLeft->SetIsHaloShadow(true);
	worldB.AddGameObject(bRight);
	worldB.AddGameObject(bLeft);
	physicsB.Update(TICK_DT);

	// Server A's owned left vs server B's shadow of the same object.
	CHECK_NEAR(aLeft->GetPhysicsObject()->GetLinearVelocity().x,
		bLeft->GetPhysicsObject()->GetLinearVelocity().x, 1e-4f);
	// And the mirror.
	CHECK_NEAR(aRight->GetPhysicsObject()->GetLinearVelocity().x,
		bRight->GetPhysicsObject()->GetLinearVelocity().x, 1e-4f);

	worldA.ClearAndErase();
	worldB.ClearAndErase();
}
