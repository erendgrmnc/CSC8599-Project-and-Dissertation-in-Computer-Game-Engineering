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

	// Counts OnCollisionEnd calls. Unregistering an object must NOT fire it: the
	// object is being removed, not separating from a contact.
	class ProbeObject : public GameObject {
	public:
		ProbeObject() : GameObject(NoSpecialFeatures, "probe") {}
		int collisionBeginCount = 0;
		int collisionEndCount = 0;
		void OnCollisionBegin(GameObject* otherObject) override {
			++collisionBeginCount;
		}
		void OnCollisionEnd(GameObject* otherObject) override {
			++collisionEndCount;
		}
	};
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
	probe->SetBoundingVolume((CollisionVolume*)new AABBVolume(Vector3(1, 1, 1)));
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

	// Guards against a vacuous pass: if no contact ever formed there is no stale
	// record to purge and the assertion below would hold for the wrong reason.
	CHECK(probe->collisionBeginCount > 0);

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
