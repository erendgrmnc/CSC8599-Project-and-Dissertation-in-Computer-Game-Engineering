#pragma once

#include "Camera.h"

#include "Transform.h"
#include "GameObject.h"

#include "AABBVolume.h"
#include "OBBVolume.h"
#include "SphereVolume.h"
#include "CapsuleVolume.h"
#include "Ray.h"

using NCL::Camera;
using namespace NCL::Maths;
using namespace NCL::CSC8503;
namespace NCL {
	class CollisionDetection
	{
	public:
		struct ContactPoint {
			Vector3 localA;
			Vector3 localB;
			Vector3 normal;
			float	penetration;
		};

		struct CollisionInfo {
			GameObject* a = nullptr;
			GameObject* b = nullptr;
			int		framesLeft = 0;

			ContactPoint point;

			CollisionInfo() {

			}

			void AddContactPoint(const Vector3& localA, const Vector3& localB, const Vector3& normal, float p) {
				point.localA		= localA;
				point.localB		= localB;
				point.normal		= normal;
				point.penetration	= p;
			}

			// Ordering key. Deliberately the world ID and NOT the pointer: mAllCollisions
			// and mBroadphaseCollisions are std::sets, so this comparator fixes the order
			// contacts are resolved in, and sequential-impulse resolution is order
			// dependent. Keying on addresses made that order depend on heap layout, so two
			// runs of the same binary with the same seed resolved the same contacts in
			// different orders and diverged.
			//
			// The previous version was also not a strict weak ordering: it collapsed the
			// pair to `(size_t)a + ((size_t)b << 32)`, which on x64 discards b's top 16
			// bits and lets the addition carry across the two halves, so two distinct
			// pairs could compare equivalent and the second would be silently dropped
			// from the set. World IDs are unique per GameWorld, so a lexicographic
			// compare on them collides only for genuinely identical pairs.
			static int OrderKey(const GameObject* o) {
				return o ? o->GetWorldID() : -1;
			}

			//Advanced collision detection / resolution
			bool operator < (const CollisionInfo& other) const {
				const int thisA = OrderKey(a);
				const int otherA = OrderKey(other.a);
				if (thisA != otherA) {
					return thisA < otherA;
				}
				return OrderKey(b) < OrderKey(other.b);
			}

			bool operator ==(const CollisionInfo& other) const {
				if (other.a == a && other.b == b) {
					return true;
				}
				return false;
			}
		};

		static bool AABBCapsuleIntersection(
			const CapsuleVolume& volumeA, const Transform& worldTransformA,
			const AABBVolume& volumeB, const Transform& worldTransformB, CollisionInfo& collisionInfo);

		static bool SphereCapsuleIntersection(
			const CapsuleVolume& volumeA, const Transform& worldTransformA,
			const SphereVolume& volumeB, const Transform& worldTransformB, CollisionInfo& collisionInfo);

		//TODO ADD THIS PROPERLY
		static bool RayBoxIntersection(const Ray&r, const Vector3& boxPos, const Vector3& boxSize, RayCollision& collision);

		static Ray BuildRayFromMouse(const PerspectiveCamera& c);

		static Ray BuidRayFromCenterOfTheCamera(const PerspectiveCamera& cam);

		static bool RayIntersection(const Ray&r, GameObject& object, RayCollision &collisions);


		static bool RayAABBIntersection(const Ray&r, const Transform& worldTransform, const AABBVolume&	volume, RayCollision& collision);
		static bool RayOBBIntersection(const Ray&r, const Transform& worldTransform, const OBBVolume&	volume, RayCollision& collision);
		static bool RaySphereIntersection(const Ray&r, const Transform& worldTransform, const SphereVolume& volume, RayCollision& collision, Vector3 position);
		static bool RayCapsuleIntersection(const Ray& r, const Transform& worldTransform, const CapsuleVolume& volume, RayCollision& collision);


		static bool RayPlaneIntersection(const Ray&r, const Plane&p, RayCollision& collisions);

		static bool	AABBTest(const Vector3& posA, const Vector3& posB, const Vector3& halfSizeA, const Vector3& halfSizeB);


		static bool ObjectIntersection(GameObject* a, GameObject* b, CollisionInfo& collisionInfo);


		static bool AABBIntersection(	const AABBVolume& volumeA, const Transform& worldTransformA,
										const AABBVolume& volumeB, const Transform& worldTransformB, CollisionInfo& collisionInfo);

		static bool SphereIntersection(	const SphereVolume& volumeA, const Transform& worldTransformA,
										const SphereVolume& volumeB, const Transform& worldTransformB, CollisionInfo& collisionInfo);

		static bool AABBSphereIntersection(	const AABBVolume& volumeA	 , const Transform& worldTransformA,
										const SphereVolume& volumeB, const Transform& worldTransformB, CollisionInfo& collisionInfo);

		// projects each OBBs onto axis using dot product
		static Vector3* GetOBBEdgeNormals(const Transform& transformA, const Transform& transformB);

		// get vertices of OBBs to check against projected axis
		static Vector3* GetOBBVertices(const OBBVolume& OBB_volume, const Transform& OBB_transform);

		static bool OBBIntersection(	const OBBVolume& volumeA, const Transform& worldTransformA,
										const OBBVolume& volumeB, const Transform& worldTransformB, CollisionInfo& collisionInfo);


		static bool OBBSphereIntersection(const OBBVolume& volumeA, const Transform& worldTransformA,
			const SphereVolume& volumeB, const Transform& worldTransformB, CollisionInfo& collisionInfo);

		static bool AABBOBBIntersection(const AABBVolume& volumeA, const Transform& worldTransformA,
			const OBBVolume& volumeB, const Transform& worldTransformB, CollisionInfo& collisionInfo);

		static bool OBBCapsuleIntersection(const CapsuleVolume& volumeA, const Transform& worldTransformA,
			const OBBVolume& volumeB, const Transform& worldTransformB, CollisionInfo& collisionInfo);


		static Vector3 Unproject(const Vector3& screenPos, const PerspectiveCamera& cam);

		static Vector3		UnprojectScreenPosition(Vector3 position, float aspect, float fov, const PerspectiveCamera&c);
		static Matrix4		GenerateInverseProjection(float aspect, float fov, float nearPlane, float farPlane);
		static Matrix4		GenerateInverseView(const Camera &c);

	protected:

	private:
		CollisionDetection()	{}
		~CollisionDetection()	{}
	};
}

