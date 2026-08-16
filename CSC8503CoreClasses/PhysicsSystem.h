#pragma once
#include "GameWorld.h"
#include "QuadTree.h"

namespace NCL {
	namespace CSC8503 {
		class PhysicsSystem	{
		public:
			PhysicsSystem(GameWorld& g);
			~PhysicsSystem();

			void Clear();

			void Update(float dt);
			void PredictFuturePositions(float dt);

			void UseGravity(bool state) {
				mApplyGravity = state;
			}

			void SetGlobalDamping(float d) {
				mGlobalDamping = d;
			}

			void SetGravity(const Vector3& g);

			void SetNewBroadphaseSize(const Vector3& levelSize);

			// Pins the substep rate to idealHZ instead of halving/doubling it from
			// measured frame cost. Required for reproducible measurement: with
			// adaptation on, two servers under different load run different
			// timesteps, and the per-substep damping term (1 - 0.4*dt) then makes
			// identical initial conditions diverge.
			void SetFixedTimestep(bool state) {
				mFixedTimestep = state;
			}

			int GetSubstepHZ() const {
				return mRealHZ;
			}

			// Lookahead horizon used when extrapolating an object's state for handoff,
			// in seconds. Must cover the server-to-server transfer latency.
			void SetPredictionHorizon(float seconds) {
				mPredictionHorizon = seconds;
			}

			float GetPredictionHorizon() const {
				return mPredictionHorizon;
			}
		protected:
			bool AreBothCollidersStatic(const CollisionDetection::CollisionInfo info);
			bool IsEitherColliderNoCollide(const CollisionDetection::CollisionInfo& info);
			
			void BasicCollisionDetection();
			void BroadPhase();
			void NarrowPhase();

			void ClearForces();

			void IntegrateAccel(float dt);
			void IntegrateVelocity(float dt);

			void UpdateConstraints(float dt);

			void UpdateCollisionList();
			void UpdateObjectAABBs();

			void PredictFutureStateOfObject(PhysicsObject& physicsObject, float dt);


			float ImpulseResolveCollision(GameObject& a , GameObject&b, CollisionDetection::ContactPoint& p) const;

			float GetCollisionElasticity(PhysicsObject objectA, PhysicsObject objectB) const;

			float CalculateMaxDt();

			float CalculateObjectWaveSpeed(const GameObject& object);

			void FrictionImpulse(GameObject& a, GameObject& b, CollisionDetection::ContactPoint& p, float j) const;

			bool GetIsCapsule(GameObject& obj) const;

			void SeperateObjects(GameObject& a, GameObject& b, CollisionDetection::ContactPoint& p, float totalMass) const;

			bool CheckFrictionShuldBeApplied(float aVelocity, float bVelocity, float totalMass) const;

			Vector3 CalculateCollisionVelocity(Vector3 contactPointA, Vector3 contactPointB, PhysicsObject* physA, PhysicsObject* physB) const;

			Vector3 CalculateFrictionDirection(Vector3 contactVelocity, Vector3 collisionNormal) const;

			float CalculateInertia(PhysicsObject* physA, PhysicsObject* physB, Vector3 contactPointA, Vector3 contactPointB, Vector3 angle) const;

			float CalculateFriction(PhysicsObject* physA, PhysicsObject* physB) const;

			Vector3 CalculateFrictionImpulse(Vector3 tangent, float j, float friction) const;

			GameWorld& mGameWorld;

			const char STATIC_COLLISION_LAYERS = StaticObj | Collectable | Zone;
			const char NO_COLLISION_RESOLUTION = Collectable | Zone;
			bool	mApplyGravity;
			Vector3 mGravity;
			float	mDTOffset;
			float	mGlobalDamping;

			std::set<CollisionDetection::CollisionInfo> mAllCollisions;
			std::set<CollisionDetection::CollisionInfo> mBroadphaseCollisions;
			std::vector<CollisionDetection::CollisionInfo> mBroadphaseCollisionsVec;
			QuadTree<GameObject*> mStaticTree;
			std::vector<GameObject*> mDynamicObjectList;

			// Replaces the old mStaticTree.Empty() sentinel for "has the one-time bulk
			// seed run?". The tree is the wrong thing to ask: a world with no static
			// geometry leaves it empty forever, so the seed re-ran on every broadphase
			// pass - and BroadPhase runs per substep, not per tick, so the duplicates
			// compounded several times a frame. It also cannot express "seeded, now
			// accepting incremental Register/Unregister".
			bool mBroadphaseSeeded = false;
			bool mUseBroadPhase		= true;
			int mNumCollisionFrames	= 5;
			int mBroadphaseX = 256;
			int mBroadphaseZ = 256;

			// The substep rate actually in use. Previously file-scope globals, which
			// meant every PhysicsSystem in a process shared one adaptive timestep.
			int   mRealHZ;
			float mRealDT;
			bool  mFixedTimestep = false;

			// Was a hardcoded 0.1f at the PredictFutureStateOfObject call site.
			float mPredictionHorizon = 0.1f;
		};
	}
}

