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
		cube->SetBoundingVolume((CollisionVolume*)new AABBVolume(Vector3(1, 1, 1)));
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
		floor->SetBoundingVolume((CollisionVolume*)new AABBVolume(Vector3(100, 1, 100)));
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
