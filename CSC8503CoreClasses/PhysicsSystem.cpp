#include "PhysicsSystem.h"
#include "PhysicsObject.h"
#include "GameObject.h"
#include "Quaternion.h"

#include "Constraint.h"
#include "CollisionDetection.h"
#include "Debug.h"
#include "Profiler.h"
#include "Window.h"
#include <algorithm>
#include <functional>
#include <iostream>
using namespace NCL;
using namespace CSC8503;

namespace {
	constexpr float SAFETY_FACTOR = 0.90f;
}

//This is the fixed timestep we'd LIKE to have
const int   idealHZ = 120;
const float idealDT = 1.0f / idealHZ;

PhysicsSystem::PhysicsSystem(GameWorld& g) : mGameWorld(g) {
	mApplyGravity = false;
	mDTOffset = 0.0f;
	mGlobalDamping = 0.995f;
	mRealHZ = idealHZ;
	mRealDT = idealDT;
	SetGravity(Vector3(0.0f, -9.8f, 0.0f));
	mStaticTree = QuadTree<GameObject*>(Vector2(mBroadphaseX, mBroadphaseZ), 7, 6);
}

PhysicsSystem::~PhysicsSystem() {
}

void PhysicsSystem::SetGravity(const Vector3& g) {
	mGravity = g;
}

void PhysicsSystem::SetNewBroadphaseSize(const Vector3& levelSize) {
	mBroadphaseX = 64;
	mBroadphaseZ = 64;
	while(levelSize.x > mBroadphaseX){
		mBroadphaseX *= 2;
	}
	while (levelSize.z > mBroadphaseZ) {
		mBroadphaseZ *= 2;
	}
	mStaticTree = QuadTree<GameObject*>(Vector2(mBroadphaseX, mBroadphaseZ), 7, 6);
}

/*

If the 'game' is ever reset, the PhysicsSystem must be
'cleared' to remove any old collisions that might still
be hanging around in the collision list. If your engine
is expanded to allow objects to be removed from the world,
you'll need to iterate through this collisions list to remove
any collisions they are in.

*/
void PhysicsSystem::Clear() {
	mAllCollisions.clear();
	mDynamicObjectList.clear();
	// Without this, a cleared world can never be re-seeded and nothing would ever
	// integrate again.
	mBroadphaseSeeded = false;
	// Anything queued refers to objects the caller is about to destroy.
	mPendingUnregister.clear();
}

/*

This is the core of the physics engine update

*/

bool useSimpleContainer = false;

int constraintIterationCount = 10;

void PhysicsSystem::Update(float dt) {
	// Must run before anything iterates mDynamicObjectList this tick.
	FlushPendingUnregisters();

	mDTOffset += dt; //We accumulate time delta here - there might be remainders from previous frame!

	GameTimer t;
	t.GetTimeDeltaSeconds();

	if (mUseBroadPhase) {
		UpdateObjectAABBs();
	}
	int iteratorCount = 0;
	while (mDTOffset > mRealDT) {
		IntegrateAccel(mRealDT); //Update accelerations from external forces
		if (mUseBroadPhase) {
			BroadPhase();
			NarrowPhase();
		}
		else {
			BasicCollisionDetection();
		}

		//This is our simple iterative solver - 
		//we just run things multiple times, slowly moving things forward
		//and then rechecking that the constraints have been met		
		float constraintDt = mRealDT / (float)constraintIterationCount;
		for (int i = 0; i < constraintIterationCount; ++i) {
			UpdateConstraints(constraintDt);
		}
		IntegrateVelocity(mRealDT); //update positions from new velocity changes

		mDTOffset -= mRealDT;
		iteratorCount++;
	}
	// Recorded before the adaptation below, which changes the rate for the NEXT
	// call and would otherwise be mistaken for this one's work.
	mLastSubstepCount = iteratorCount;

	ClearForces();	//Once we've finished with the forces, reset them to zero

	UpdateCollisionList(); //Remove any old collisions

	t.Tick();
	float updateTime = t.GetTimeDeltaSeconds();

	// Measurement runs pin the substep rate so that servers under different load
	// still integrate with the same dt (see SetFixedTimestep).
	if (mFixedTimestep) {
		return;
	}

	//Uh oh, physics is taking too long...
	if (updateTime > mRealDT) {
		mRealHZ /= 2;
		mRealDT *= 2;
		//std::cout << "Dropping iteration count due to long physics time...(now " << mRealHZ << ")\n";
	}
	else if (dt * 2 < mRealDT) { //we have plenty of room to increase iteration count!
		int temp = mRealHZ;
		mRealHZ *= 2;
		mRealDT /= 2;

		if (mRealHZ > idealHZ) {
			mRealHZ = idealHZ;
			mRealDT = idealDT;
		}
		if (temp != mRealHZ) {
			//std::cout << "Raising iteration count due to short physics time...(now " << mRealHZ << ")\n";
		}
	}
}

/*
Later on we're going to need to keep track of collisions
across multiple frames, so we store them in a set.

The first time they are added, we tell the objects they are colliding.
The frame they are to be removed, we tell them they're no longer colliding.

From this simple mechanism, we we build up gameplay interactions inside the
OnCollisionBegin / OnCollisionEnd functions (removing health when hit by a
rocket launcher, gaining a point when the player hits the gold coin, and so on).
*/
void PhysicsSystem::UpdateCollisionList() {
	for (std::set<CollisionDetection::CollisionInfo>::iterator i = mAllCollisions.begin(); i != mAllCollisions.end(); ) {
		if ((*i).framesLeft == mNumCollisionFrames) {
			i->a->OnCollisionBegin(i->b);
			i->b->OnCollisionBegin(i->a);
		}

		CollisionDetection::CollisionInfo& in = const_cast<CollisionDetection::CollisionInfo&>(*i);
		in.framesLeft--;

		if ((*i).framesLeft < 0) {
			i->a->OnCollisionEnd(i->b);
			i->b->OnCollisionEnd(i->a);
			i = mAllCollisions.erase(i);
		}
		else {
			++i;
		}
	}
}

void PhysicsSystem::SetWorkerThreadCount(int workerCount) {
	if (workerCount <= 0) {
		mTaskPool.reset();
		mPairBuffers.clear();
		return;
	}
	mTaskPool = std::make_unique<TaskPool>(workerCount);
	mPairBuffers.assign(mTaskPool->GetBufferCount(), {});
	std::cout << "Physics worker threads: " << mTaskPool->GetWorkerCount()
		<< " (+ the calling thread)\n";
}

void PhysicsSystem::UpdateObjectAABBs() {
	// Parallel over the dynamic list, serial over the rest. Each object writes only
	// its own AABB, so there is nothing shared to guard.
	//
	// Deliberately NOT OperateOnContents over the whole world: static geometry is
	// seeded into the quadtree once and never moves, so recomputing its AABB every
	// tick was wasted work that also could not be split by index.
	if (mTaskPool && !mDynamicObjectList.empty()) {
		mTaskPool->ParallelFor(static_cast<int>(mDynamicObjectList.size()),
			[this](int begin, int end, int) {
				for (int i = begin; i < end; ++i) {
					mDynamicObjectList[i]->UpdateBroadphaseAABB();
				}
			});
		// Statics still need one pass, since the seed loop reads their AABBs.
		if (!mBroadphaseSeeded) {
			mGameWorld.OperateOnContents([](GameObject* g) { g->UpdateBroadphaseAABB(); });
		}
		return;
	}

	mGameWorld.OperateOnContents(
		[](GameObject* g) {
			g->UpdateBroadphaseAABB();
		}
	);
}

void PhysicsSystem::PredictFutureStateOfObject(PhysicsObject& physicsObject, float dt) {
	Vector3 linearVel = physicsObject.GetLinearVelocity();
	Vector3 force = physicsObject.GetForce();
	const float inverseMass = physicsObject.GetInverseMass();
	auto transform = physicsObject.GetTransform();

	Vector3 accel = force * inverseMass;
	if (inverseMass > 0) {
		accel += mGravity;
	}
	linearVel += accel * dt;
	Vector3 predictedPosition = transform->GetPosition() + linearVel * dt;
	transform->SetPredictedPosition(predictedPosition);

	Vector3 angVel = physicsObject.GetAngularVelocity();
	Vector3 torque = physicsObject.GetTorque();
	Matrix3 inertiaTensor = physicsObject.GetInverseInertiaTensor();
	physicsObject.UpdateInertiaTensor();
	Vector3 angAccel = inertiaTensor * torque;
	angVel += angAccel * dt;

	Quaternion orientation = transform->GetOrientation();
	Quaternion angVelocityQuat(angVel * dt * 0.5f, 0.0f);
	Quaternion predictedOrientation = orientation + (angVelocityQuat * orientation);
	predictedOrientation.Normalise();

	transform->SetPredictedOrientation(predictedOrientation);
}

void PhysicsSystem::PredictFuturePositions(float dt) {
	// Calculate maximum allowed time step based on stability criteria
	float maxDt = CalculateMaxDt();

	// Apply safety factor
	maxDt *= SAFETY_FACTOR;

	// Adjust time step
	dt = std::min(dt, maxDt);

	// The handoff lookahead horizon. This is the quantity the predictive-handoff
	// correctness argument is stated in terms of, so it is a declared parameter
	// rather than a literal at the call site. Note the clamped dt above is
	// deliberately NOT used: the horizon must cover the transfer latency, not the
	// local substep.
	const float horizon = mPredictionHorizon;

	for (auto& obj : mDynamicObjectList) {
		if (obj->GetPhysicsObject() != nullptr && obj->IsNetworkActive()) {
			PredictFutureStateOfObject(*obj->GetPhysicsObject(), horizon);
		}
	}
}

/*

This is how we'll be doing collision detection in tutorial 4.
We step thorugh every pair of objects once (the inner for loop offset
ensures this), and determine whether they collide, and if so, add them
to the collision set for later processing. The set will guarantee that
a particular pair will only be added once, so objects colliding for
multiple frames won't flood the set with duplicates.
*/
void PhysicsSystem::BasicCollisionDetection() {
	std::vector<GameObject*>::const_iterator first;
	std::vector<GameObject*>::const_iterator last;
	mGameWorld.GetObjectIterators(first, last);

	for (auto i = first; i != last; i++) {
		if ((*i)->GetPhysicsObject() == nullptr)
			continue;
		for (auto j = i + 1; j != last; j++) {
			if ((*j)->GetPhysicsObject() == nullptr)
				continue;
			CollisionDetection::CollisionInfo info;
			if (CollisionDetection::ObjectIntersection(*i, *j, info)) {
				if (!((*i)->GetBoundingVolume()->applyPhysics && (*j)->GetBoundingVolume()->applyPhysics))
					continue;
				float j = ImpulseResolveCollision(*info.a, *info.b, info.point);
				FrictionImpulse(*info.a, *info.b, info.point, j);
				info.framesLeft = mNumCollisionFrames;
				mAllCollisions.insert(info);
			}
		}
	}
}

/*

In tutorial 5, we start determining the correct response to a collision,
so that objects separate back out.

*/
float PhysicsSystem::ImpulseResolveCollision(GameObject& a, GameObject& b, CollisionDetection::ContactPoint& p) const {
	PhysicsObject* physA = a.GetPhysicsObject();
	PhysicsObject* physB = b.GetPhysicsObject();

	float totalMass = physA->GetInverseMass() + physB->GetInverseMass();

	// both objects are static
	if (totalMass == 0)
		return 0;

	SeperateObjects(a, b, p, totalMass);

	// local collision points
	Vector3 relativeA = p.localA;
	Vector3 relativeB = p.localB;

	Vector3 contactVelocity = CalculateCollisionVelocity(relativeA, relativeB, physA, physB);

	float impulseForce = Vector3::Dot(contactVelocity, p.normal);
	float angularEffect = CalculateInertia(physA, physB, relativeA, relativeB, p.normal);

	float cRestitution = GetCollisionElasticity(*physA, *physB); // loss of kinetic energy

	float j = (-(1.0f + cRestitution) * impulseForce) / (totalMass + angularEffect);
	Vector3 fullImpulse = p.normal * j;

	// apply impulse in opposite directions for collision responce
	physA->ApplyLinearImpulse(-fullImpulse);
	physB->ApplyLinearImpulse(fullImpulse);
	physA->ApplyAngularImpulse(Vector3::Cross(relativeA, -fullImpulse));
	physB->ApplyAngularImpulse(Vector3::Cross(relativeB, fullImpulse));

	return j;
}

float PhysicsSystem::GetCollisionElasticity(PhysicsObject objectA, PhysicsObject objectB) const {
	return objectA.GetElasticity() * objectB.GetElasticity();
}

float PhysicsSystem::CalculateMaxDt() {
	float maxVelocity = 0.0f;
	for (auto& obj : mDynamicObjectList) {
		if (obj->GetPhysicsObject() != nullptr) {
			maxVelocity = std::max(maxVelocity, obj->GetPhysicsObject()->GetLinearVelocity().Length());
		}
	}

	// Replace this with a suitable value based on your simulation

	// Avoid division by zero
	if (maxVelocity > 0.0f) {
		constexpr float maxAllowedVelocity = INT_MAX; // TODO(erendgrmnc): can be causing errors!!!
		return maxAllowedVelocity / maxVelocity;
	}
	else {
		return 1.0f; // Default value if no objects are moving
	}
}

void PhysicsSystem::FrictionImpulse(GameObject& a, GameObject& b, CollisionDetection::ContactPoint& p, float j) const {
	PhysicsObject* physA = a.GetPhysicsObject();
	PhysicsObject* physB = b.GetPhysicsObject();
	float totalMass = physA->GetInverseMass() + physB->GetInverseMass();
	if (!CheckFrictionShuldBeApplied(physA->GetLinearVelocity().Length(), physB->GetLinearVelocity().Length(), totalMass))
		return;
	Vector3 relativeA = p.localA;
	Vector3 relativeB = p.localB;

	Vector3 contactVelocity = CalculateCollisionVelocity(relativeA, relativeB, physA, physB);

	Vector3 tangent = CalculateFrictionDirection(contactVelocity, p.normal);
	float angularEffect = CalculateInertia(physA, physB, relativeA, relativeB, tangent);

	float impulseForce = Vector3::Dot(contactVelocity, tangent);

	j = abs(j);

	float friction = CalculateFriction(physA, physB);

	Vector3 fullFrictionImpulse = CalculateFrictionImpulse(tangent, j, friction);

	// apply impulse opposite direction incoming force
	physA->ApplyLinearImpulse(fullFrictionImpulse);
	physB->ApplyLinearImpulse(-fullFrictionImpulse);
	if (!(GetIsCapsule(a)))
		physA->ApplyAngularImpulse(Vector3::Cross(relativeA, fullFrictionImpulse));
	if (!(GetIsCapsule(b)))
		physB->ApplyAngularImpulse(Vector3::Cross(relativeB, -fullFrictionImpulse));
}

bool PhysicsSystem::GetIsCapsule(GameObject& obj) const {
	if (obj.GetBoundingVolume()->type == VolumeType::Capsule)
		return true;
	return false;
}

void PhysicsSystem::SeperateObjects(GameObject& a, GameObject& b, CollisionDetection::ContactPoint& p, float totalMass) const {
	Transform& transformA = a.GetTransform();
	Transform& transformB = b.GetTransform();

	transformA.SetPosition(transformA.GetPosition() - (p.normal * p.penetration * (a.GetPhysicsObject()->GetInverseMass() / totalMass)));
	transformB.SetPosition(transformB.GetPosition() + (p.normal * p.penetration * (b.GetPhysicsObject()->GetInverseMass() / totalMass)));
}

bool PhysicsSystem::CheckFrictionShuldBeApplied(float aVelocity, float bVelocity, float totalMass) const {
	if (aVelocity < 1 && bVelocity < 1)
		return false;
	if (totalMass <= 0)
		return false;
	return true;
}

Vector3 PhysicsSystem::CalculateCollisionVelocity(Vector3 contactPointA, Vector3 contactPointB, PhysicsObject* physA, PhysicsObject* physB) const {
	Vector3 angVelocityA = Vector3::Cross(physA->GetAngularVelocity(), contactPointA);
	Vector3 angVelocityB = Vector3::Cross(physB->GetAngularVelocity(), contactPointB);

	Vector3 fullVelocityA = physA->GetLinearVelocity() + angVelocityA;
	Vector3 fullVelocityB = physB->GetLinearVelocity() + angVelocityB;

	return fullVelocityB - fullVelocityA;
}

Vector3 PhysicsSystem::CalculateFrictionDirection(Vector3 contactVelocity, Vector3 collisionNormal) const {
	// get direction of friction
	float impulseForce = Vector3::Dot(contactVelocity, collisionNormal);
	Vector3 tangent = contactVelocity - (collisionNormal * impulseForce);
	tangent.Normalise();
	return tangent;
}

float PhysicsSystem::CalculateInertia(PhysicsObject* physA, PhysicsObject* physB, Vector3 contactPointA, Vector3 contactPointB, Vector3 angle) const {
	Vector3 inertiaA = Vector3::Cross(physA->GetInverseInertiaTensor() * Vector3::Cross(contactPointA, angle), contactPointA);
	Vector3 inertiaB = Vector3::Cross(physB->GetInverseInertiaTensor() * Vector3::Cross(contactPointB, angle), contactPointB);
	return Vector3::Dot(inertiaA + inertiaB, angle);
}

float PhysicsSystem::CalculateFriction(PhysicsObject* physA, PhysicsObject* physB) const {
	float frictionA;
	float frictionB;

	if (physA->GetForce() == Vector3(0, 0, 0) && physA->GetInverseMass() > 0)
		frictionA = physA->GetStaticFriction();
	else
		frictionA = physA->GetDynamicFriction();

	if (physB->GetForce() == Vector3(0, 0, 0) && physB->GetInverseMass() > 0)
		frictionB = physB->GetStaticFriction();
	else
		frictionB = physB->GetDynamicFriction();
	return (frictionA + frictionB) / 2;
}

Vector3 PhysicsSystem::CalculateFrictionImpulse(Vector3 tangent, float j, float friction) const {
	Vector3 fullFrictionImpulse;

	fullFrictionImpulse = tangent * j * friction;

	return fullFrictionImpulse;
}

/*

Later, we replace the BasicCollisionDetection method with a broadphase
and a narrowphase collision detection method. In the broad phase, we
split the world up using an acceleration structure, so that we can only
compare the collisions that we absolutely need to.

*/
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

void PhysicsSystem::BroadPhase() {
	// clear last frames collisions
 	mBroadphaseCollisions.clear();

	// create quadtree to store all objects
	std::vector<GameObject*>::const_iterator first;
	std::vector<GameObject*>::const_iterator last;
	mGameWorld.GetObjectIterators(first, last);
	if (first == last) return;
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
	for (int i = 0; i < mDynamicObjectList.size(); i++) {
		if (!mDynamicObjectList[i]->HasPhysics()) continue;
		Vector3 halfSize;
		mDynamicObjectList[i]->GetBroadphaseAABB(halfSize);
		mStaticTree.OperateOnLeaf([&](std::list<QuadTreeEntry<GameObject*>>& data) {
			CollisionDetection::CollisionInfo info;
			for (auto j = data.begin(); j != data.end(); j++) {
				if (!(*j).object->HasPhysics()) continue;
				// Same canonical order the set comparator uses, for the same reason as
				// the dynamic/dynamic pass below. std::min/std::max on GameObject*
				// orders by ADDRESS, so which body became `a` - and therefore the
				// direction of the contact normal and the operand order of the impulse
				// arithmetic - depended on heap layout.
				GameObject* dynamicObj = mDynamicObjectList[i];
				GameObject* staticObj = (*j).object;
				const bool staticFirst = CollisionDetection::CollisionInfo::OrderKey(staticObj)
					< CollisionDetection::CollisionInfo::OrderKey(dynamicObj);
				info.a = staticFirst ? staticObj : dynamicObj;
				info.b = staticFirst ? dynamicObj : staticObj;
				Vector3 halfSizeA;
				Vector3 halfSizeB;
				info.a->GetBroadphaseAABB(halfSizeA);
				info.b->GetBroadphaseAABB(halfSizeB);
				halfSizeA.y = 1000.0f;
				halfSizeB.y = 1000.0f;
				if (!CollisionDetection::AABBTest(info.a->GetTransform().GetPosition() + info.a->GetBoundingVolume()->GetOffset(), 
					info.b->GetTransform().GetPosition() + info.b->GetBoundingVolume()->GetOffset(),
					halfSizeA, halfSizeB)) continue;
				if (mDynamicObjectList[i]->GetCollisionLayer() & Npc && (*j).object->GetCollisionLayer() & Collectable) {
					continue;
				}
				mBroadphaseCollisions.insert(info);
			}
			}, mDynamicObjectList[i]->GetTransform().GetPosition(), halfSize);
	}

	// The dynamic/dynamic pass, no longer quadratic.
	BroadPhaseDynamicPairs();
}

// Dynamic/dynamic candidate pairs via a uniform XZ grid.
//
// This replaces two nested loops over mDynamicObjectList, which tested every pair and
// so cost O(n^2) in the number of objects a server owns. Measured before the change:
// 8.63 ms per tick at 1,000 objects and 36.72 ms at 2,000 - an exponent of 2.09
// against an 8.33 ms budget at 120 Hz. Contacts only doubled across that range, so
// the cost was pair ENUMERATION rather than contact resolution.
//
// It produces exactly the same SET of pairs the quadratic scan did. The grid only
// decides which pairs are tested; the AABB test, the canonical a/b ordering and the
// destination set are unchanged, so contact resolution order - which
// mBroadphaseCollisions fixes through its comparator - is unaffected and results stay
// bit-identical.
void PhysicsSystem::BroadPhaseDynamicPairs() {
	const int objectCount = static_cast<int>(mDynamicObjectList.size());
	if (objectCount < 2) {
		return;
	}

	// Cell size from the largest object present, so one object never spans more than a
	// couple of cells. A fixed size would degenerate whenever the world contained
	// something much bigger than the constant assumed.
	float maxExtent = 0.0f;
	for (int i = 0; i < objectCount; ++i) {
		if (!mDynamicObjectList[i]->HasPhysics()) continue;
		Vector3 halfSize;
		if (!mDynamicObjectList[i]->GetBroadphaseAABB(halfSize)) continue;
		maxExtent = std::max(maxExtent, std::max(halfSize.x, halfSize.z));
	}
	mBroadphaseGrid.cellSize = std::max(1.0f, maxExtent * 4.0f);
	const float invCell = 1.0f / mBroadphaseGrid.cellSize;

	// Cleared rather than reconstructed: the buckets keep their capacity, so a steady
	// state costs no allocation at all after the first tick.
	for (auto& cell : mBroadphaseGrid.cells) {
		cell.second.clear();
	}

	auto cellKey = [](int cx, int cz) -> long long {
		// Two 32-bit cell coordinates packed into one key. Interleaving or hashing
		// would be no better here: the map is already a hash table.
		return (static_cast<long long>(cx) << 32) ^ static_cast<unsigned int>(cz);
	};

	// Insert by the object's AABB span, not just its centre, so an object larger than
	// a cell is found from every cell it overlaps.
	for (int i = 0; i < objectCount; ++i) {
		GameObject* object = mDynamicObjectList[i];
		if (!object->HasPhysics()) continue;
		Vector3 halfSize;
		if (!object->GetBroadphaseAABB(halfSize)) continue;

		const Vector3 centre = object->GetTransform().GetPosition()
			+ object->GetBoundingVolume()->GetOffset();
		const int minCellX = static_cast<int>(std::floor((centre.x - halfSize.x) * invCell));
		const int maxCellX = static_cast<int>(std::floor((centre.x + halfSize.x) * invCell));
		const int minCellZ = static_cast<int>(std::floor((centre.z - halfSize.z) * invCell));
		const int maxCellZ = static_cast<int>(std::floor((centre.z + halfSize.z) * invCell));

		for (int cx = minCellX; cx <= maxCellX; ++cx) {
			for (int cz = minCellZ; cz <= maxCellZ; ++cz) {
				mBroadphaseGrid.cells[cellKey(cx, cz)].push_back(i);
			}
		}
	}

	// Each object against the objects in its own and neighbouring cells. Only j > i is
	// considered, which is what stops a pair being tested twice - the same rule the
	// quadratic scan used, and the reason an object spanning several cells cannot
	// produce duplicates either.
	//
	// PARALLEL, but only the COLLECTION. Each thread appends the pairs it finds to its
	// own buffer and nothing is shared; the buffers are then merged into
	// mBroadphaseCollisions on one thread, below. Inserting into the set directly from
	// several threads would be a data race, and - worse - the merge order would vary.
	//
	// The result is identical either way, because mBroadphaseCollisions is a std::set
	// ordered by the contact-order comparator: the SET does not depend on the order
	// things were inserted, and NarrowPhase walks it in comparator order. That is what
	// makes this safe to parallelise while contact resolution is not.
	//
	// The candidate scratch buffer has to be per thread too, so it moves out of
	// mBroadphaseGrid and into the lambda.
	const auto collectRange = [&](int begin, int end, int worker) {
	std::vector<CollisionDetection::CollisionInfo>& pairs = mPairBuffers[worker];
	std::vector<int> candidates;
	for (int i = begin; i < end; ++i) {
		GameObject* objectA = mDynamicObjectList[i];
		if (!objectA->HasPhysics()) continue;
		Vector3 halfSizeA;
		if (!objectA->GetBroadphaseAABB(halfSizeA)) continue;

		const Vector3 centreA = objectA->GetTransform().GetPosition()
			+ objectA->GetBoundingVolume()->GetOffset();
		const int minCellX = static_cast<int>(std::floor((centreA.x - halfSizeA.x) * invCell));
		const int maxCellX = static_cast<int>(std::floor((centreA.x + halfSizeA.x) * invCell));
		const int minCellZ = static_cast<int>(std::floor((centreA.z - halfSizeA.z) * invCell));
		const int maxCellZ = static_cast<int>(std::floor((centreA.z + halfSizeA.z) * invCell));

		candidates.clear();
		for (int cx = minCellX - 1; cx <= maxCellX + 1; ++cx) {
			for (int cz = minCellZ - 1; cz <= maxCellZ + 1; ++cz) {
				const auto cell = mBroadphaseGrid.cells.find(cellKey(cx, cz));
				if (cell == mBroadphaseGrid.cells.end()) continue;
				for (int j : cell->second) {
					if (j > i) {
						candidates.push_back(j);
					}
				}
			}
		}
		if (candidates.empty()) continue;

		// An object spanning several cells appears in each of them, so the same
		// neighbour can be collected more than once. Sorting and uniquing is cheaper
		// than a per-object visited set and keeps the work proportional to the
		// candidates actually found.
		std::sort(candidates.begin(), candidates.end());
		candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

		for (int j : candidates) {
			GameObject* objectB = mDynamicObjectList[j];
			if (!objectB->HasPhysics()) continue;

			CollisionDetection::CollisionInfo info;
			// Canonicalise the pair by its GLOBAL id, not by address and not by world
			// id. Which body ends up as `a` decides the contact normal's direction and
			// which side takes +impulse, so ordering on pointers made the resolved
			// result depend on heap layout - and ordering on world ids made a
			// cross-border pair come out oriented differently on the two servers that
			// share it, since the world id is a local creation counter.
			GameObject* first = objectA;
			GameObject* second = objectB;
			if (CollisionDetection::CollisionInfo::OrderKey(second)
				< CollisionDetection::CollisionInfo::OrderKey(first)) {
				std::swap(first, second);
			}
			info.a = first;
			info.b = second;

			Vector3 boundsA;
			Vector3 boundsB;
			info.a->GetBroadphaseAABB(boundsA);
			info.b->GetBroadphaseAABB(boundsB);
			// Y is deliberately ignored, exactly as the quadratic scan did. This is why
			// a two-dimensional grid loses nothing.
			boundsA.y = 1000.0f;
			boundsB.y = 1000.0f;
			if (!CollisionDetection::AABBTest(
				info.a->GetTransform().GetPosition() + info.a->GetBoundingVolume()->GetOffset(),
				info.b->GetTransform().GetPosition() + info.b->GetBoundingVolume()->GetOffset(),
				boundsA, boundsB)) continue;

			pairs.push_back(info);
		}
	}
	};

	const int bufferCount = mTaskPool ? mTaskPool->GetBufferCount() : 1;
	if (static_cast<int>(mPairBuffers.size()) < bufferCount) {
		mPairBuffers.resize(bufferCount);
	}
	for (auto& buffer : mPairBuffers) {
		buffer.clear();
	}

	if (mTaskPool) {
		mTaskPool->ParallelFor(objectCount, collectRange);
	}
	else {
		collectRange(0, objectCount, 0);
	}

	// Merged on one thread, in buffer order. The destination is an ordered set, so the
	// merge order cannot affect the result - but doing it here rather than inside the
	// workers is what removes the race.
	for (const auto& buffer : mPairBuffers) {
		for (const CollisionDetection::CollisionInfo& info : buffer) {
			mBroadphaseCollisions.insert(info);
		}
	}
}



/*

The broadphase will now only give us likely collisions, so we can now go through them,
and work out if they are truly colliding, and if so, add them into the main collision list
*/
void PhysicsSystem::NarrowPhase() {
	int resolved = 0;
	// iteratr through all collisions added and if collision then call impulse resolve collision
	for (std::set<CollisionDetection::CollisionInfo>::iterator i = mBroadphaseCollisions.begin(); i != mBroadphaseCollisions.end(); i++) {
		CollisionDetection::CollisionInfo info = *i;

		if (CollisionDetection::ObjectIntersection(info.a, info.b, info)) {
			++resolved;
			info.framesLeft = mNumCollisionFrames;
			if (!(info.a->GetCollisionLayer() & NO_COLLISION_RESOLUTION || info.b->GetCollisionLayer() & NO_COLLISION_RESOLUTION)) {
				float j = ImpulseResolveCollision(*info.a, *info.b, info.point);
				FrictionImpulse(*info.a, *info.b, info.point, j);
			}
			mAllCollisions.insert(info);
		}
	}
	Profiler::SetContactsResolved(resolved);
}

/*
Integration of acceleration and velocity is split up, so that we can
move objects multiple times during the course of a PhysicsUpdate,
without worrying about repeated forces accumulating etc.

This function will update both linear and angular acceleration,
based on any forces that have been accumulated in the objects during
the course of the previous game frame.
*/
void PhysicsSystem::IntegrateAccel(float dt) {
	// Independent per object: each iteration writes only to the object it is
	// processing, so splitting the range changes nothing about the result. The
	// integrated count is the only shared state, and it is a count - accumulated
	// atomically rather than merged, since its value does not depend on order.
	std::atomic<int> integratedCount{ 0 };

	const auto integrateRange = [&](int begin, int end, int) {
		int integrated = 0;
		for (int i = begin; i < end; i++) {
			// Skip deactivated objects, matching BroadPhase. In the distributed build
			// every server pre-seeds the whole world and deactivates the objects outside
			// its own region; without this test each server integrated every object in
			// the world, so per-server physics cost scaled with total world size instead
			// of region occupancy.
			if (!mDynamicObjectList[i]->HasPhysics())
				continue;
			// A halo shadow has physics so that the broadphase pairs with it, but it is
			// owned by another server and that server integrates it. Integrating it here
			// too would make this server a second owner - invariant I7 - and the two
			// copies would diverge within a tick.
			if (mDynamicObjectList[i]->IsHaloShadow())
				continue;
			PhysicsObject* object = mDynamicObjectList[i]->GetPhysicsObject();
			if (object == nullptr)
				continue;
			++integrated;
			// inverse mass for multiplication instead of division and unmoving object
			float inverseMass = object->GetInverseMass();

			Vector3 linearVel = object->GetLinearVelocity();
			Vector3 force = object->GetForce();

			Vector3 accel = force * inverseMass;

			if (mApplyGravity && inverseMass > 0)
				accel += mGravity;

			linearVel += accel * dt;
			object->SetLinearVelocity(linearVel);

			// get objects current torque and angular velocity
			Vector3 torque = object->GetTorque();
			Vector3 angVel = object->GetAngularVelocity();

			// update objects orientation
			object->UpdateInertiaTensor();

			// get angular accel using new orientation * torque
			Vector3 angAccel = object->GetInverseInertiaTensor() * torque;
			// scale by dt and set as new angular velocity
			angVel += angAccel * dt;
			object->SetAngularVelocity(angVel);
		}
		integratedCount.fetch_add(integrated, std::memory_order_relaxed);
	};

	const int count = static_cast<int>(mDynamicObjectList.size());
	if (mTaskPool) {
		mTaskPool->ParallelFor(count, integrateRange);
	}
	else {
		integrateRange(0, count, 0);
	}

	// Compared against the owned-object count in telemetry: a mismatch means this
	// server is integrating objects outside its own region.
	Profiler::SetIntegratedObjects(integratedCount.load(std::memory_order_relaxed));
}

/*
This function integrates linear and angular velocity into
position and orientation. It may be called multiple times
throughout a physics update, to slowly move the objects through
the world, looking for collisions.
*/
void PhysicsSystem::IntegrateVelocity(float dt) {
	// Independent per object, exactly as IntegrateAccel is: position and orientation
	// are written only on the object being processed.
	const float frameLinearDampening = 1.0f - (0.4f * dt);

	const auto integrateRange = [&](int begin, int end, int) {
		for (int i = begin; i < end; i++) {
			// See IntegrateAccel: only objects this server owns are integrated, and a
			// halo shadow is owned elsewhere.
			if (!mDynamicObjectList[i]->HasPhysics())
				continue;
			if (mDynamicObjectList[i]->IsHaloShadow())
				continue;
			PhysicsObject* object = mDynamicObjectList[i]->GetPhysicsObject();
			if (object == nullptr)
				continue;
			// determine position
			Transform& transform = mDynamicObjectList[i]->GetTransform();
			Vector3 position = transform.GetPosition();
			Vector3 linearVel = object->GetLinearVelocity();
			position += linearVel * dt;
			transform.SetPosition(position);
			// linear dampening
			linearVel = linearVel * frameLinearDampening;
			object->SetLinearVelocity(linearVel);

			// orientation
			Quaternion orientation = transform.GetOrientation();
			Vector3 angVel = object->GetAngularVelocity();
			orientation = orientation + (Quaternion(angVel * dt * 0.5f, 0.0f) * orientation);
			orientation.Normalise();
			transform.SetOrientation(orientation);

			// dampen new angular velocity
			float frameAngularDamping = 1.0f - (0.4f * dt);
			angVel = angVel * frameAngularDamping;
			object->SetAngularVelocity(angVel);
		}
	};

	const int count = static_cast<int>(mDynamicObjectList.size());
	if (mTaskPool) {
		mTaskPool->ParallelFor(count, integrateRange);
	}
	else {
		integrateRange(0, count, 0);
	}
}

/*
Once we're finished with a physics update, we have to
clear out any accumulated forces, ready to receive new
ones in the next 'game' frame.
*/
void PhysicsSystem::ClearForces() {
	mGameWorld.OperateOnContents(
		[](GameObject* o) {
			// Objects without a PhysicsObject are legal in the world (and will be
			// more common once objects can be spawned at runtime); IntegrateAccel
			// already guards for this.
			if (o->GetPhysicsObject() == nullptr)
				return;
			o->GetPhysicsObject()->ClearForces();
		}
	);
}


/*

As part of the final physics tutorials, we add in the ability
to constrain objects based on some extra calculation, allowing
us to model springs and ropes etc.

*/
void PhysicsSystem::UpdateConstraints(float dt) {
	std::vector<Constraint*>::const_iterator first;
	std::vector<Constraint*>::const_iterator last;
	mGameWorld.GetConstraintIterators(first, last);

	for (auto i = first; i != last; ++i) {
		(*i)->UpdateConstraint(dt);
	}
}