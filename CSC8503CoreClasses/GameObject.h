#pragma once
#include "Transform.h"
#include "CollisionVolume.h"

using std::vector;

namespace NCL::CSC8503 {
	class NetworkObject;
	class PhysicsObject;
	class RenderObject;
#ifndef DISTRIBUTEDSYSTEMACTIVE
	class SoundObject;
#endif

	enum CollisionLayer {
		StaticObj = 1,
		Collectable = 2,
		Player = 4,
		Npc = 8,
		NoCollide = 16,
		Zone = 32,
		NoSpecialFeatures = 64
	};

	class GameObject {
	public:
		GameObject(CollisionLayer = NoSpecialFeatures, const std::string& name = "");
		virtual ~GameObject();

		enum GameObjectState {
			Idle,
			Walk,
			Sprint,
			IdleCrouch,
			Crouch,
			Happy,
			Point,
			Default
		};

		void SetBoundingVolume(CollisionVolume* vol) {
			mBoundingVolume = vol;
		}

		const CollisionVolume* GetBoundingVolume() const {
			return mBoundingVolume;
		}

		bool IsRendered() const {
			return mIsRendered;
		}

		bool HasPhysics() const {
			return mHasPhysics;
		}

		bool IsNetworkActive() const {
			return mIsNetworkActive;
		}

		// --- halo shadow ---
		//
		// A read-only copy of an object owned by a NEIGHBOURING server, held so that
		// objects either side of a region border can collide. It is deliberately not
		// expressible as a combination of the flags above: it must have physics (or
		// the broadphase never forms a pair with it) while not being simulated (or two
		// servers integrate the same object, which is a second owner), and those two
		// are the same flag today.
		//
		// Everything that treats an object as this server's responsibility - the
		// integrator, the border check, the snapshot loop, command targeting - must
		// test this and skip. See the table in the halo band spec.
		bool IsHaloShadow() const {
			return mIsHaloShadow;
		}

		void SetIsHaloShadow(bool isShadow) {
			mIsHaloShadow = isShadow;
		}

		// --- player control (increment 7) ---
		//
		// Continuous movement input is STATE, not an event: it is applied every tick
		// until superseded, never sequenced and never relayed. It therefore has to
		// travel with the object across a handoff, or a driven avatar stalls on every
		// border crossing until the client's next axis update reaches the new owner.
		int GetControllerPlayerID() const {
			return mControllerPlayerID;
		}

		const Vector3& GetMoveAxis() const {
			return mMoveAxis;
		}

		void SetControlState(int playerID, const Vector3& axis) {
			mControllerPlayerID = playerID;
			mMoveAxis = axis;
		}

		void SetIsRendered(bool isRendered) {
			mIsRendered = isRendered;
		}

		void SetHasPhysics(bool hasPhysics) {
			mHasPhysics = hasPhysics;
		}

		void ToggleIsRendered() {
			mIsRendered = !mIsRendered;
		}

		void ToggleHasPhysics() {
			mHasPhysics = !mHasPhysics;
		}

		void SetActive(bool isActive) {
			mIsRendered = isActive;
			mHasPhysics = isActive;
			mIsNetworkActive = isActive;
		}

		bool IsActive() {
			return mIsRendered && mHasPhysics;
		}

		Transform& GetTransform() {
			return mTransform;
		}

		RenderObject* GetRenderObject() const {
			return mRenderObject;
		}

		PhysicsObject* GetPhysicsObject() const {
			return mPhysicsObject;
		}

		NetworkObject* GetNetworkObject() const {
			return mNetworkObject;
		}

#ifndef DISTRIBUTEDSYSTEMACTIVE
		SoundObject* GetSoundObject() const {
			return mSoundObject;
		}

		void SetSoundObject(SoundObject* newObject) {
			mSoundObject = newObject;
		}
#endif


#ifdef USEGL
		void SetIsSensed(bool sensed);

		bool GetIsSensed();
#endif


		void SetNetworkObject(NetworkObject* netObj);


		void SetRenderObject(RenderObject* newObject) {
			mRenderObject = newObject;
		}

		void SetPhysicsObject(PhysicsObject* newObject) {
			mPhysicsObject = newObject;
			mHasPhysics = true;
		}

		const std::string& GetName() const {
			return mName;
		}

		virtual void OnCollisionBegin(GameObject* otherObject) {
			//std::cout << "OnCollisionBegin event occured! between " << mName << "and" << otherObject->GetName() << "\n";
		}

		virtual void OnCollisionEnd(GameObject* otherObject) {
			//std::cout << "OnCollisionEnd event occured!\n";
		}

		bool GetBroadphaseAABB(Vector3& outsize) const;

		void UpdateBroadphaseAABB();

		void SetWorldID(int newID) {
			mWorldID = newID;
		}

		int		GetWorldID() const {
			return mWorldID;
		}

		// --- contact ordering identity ---
		//
		// The id used to decide which body of a contact pair is `a`, which fixes the
		// contact normal's direction and the operand order of the impulse arithmetic.
		//
		// It CANNOT be the world ID. That is a per-GameWorld creation-order counter
		// (GameWorld::AddGameObject does worldIDCounter++), so two servers assign
		// different world IDs to the same pair: on the owning server the object is
		// built at pre-seed and its neighbour's shadow is built later, and on the
		// other server it is the other way round. The pair would then be oriented
		// oppositely on the two servers, and each would compute a slightly different
		// impulse from the same contact - invariant I8, broken silently and only for
		// pairs that straddle a border.
		//
		// Network ids are globally unique and identical on every server, so they are
		// the right identity. Set automatically with the network object, and set
		// explicitly on a halo shadow, which deliberately has no network object.
		// -1 for objects with no global identity - static geometry, the floor - which
		// sort after every networked object and fall back to the world ID.
		int GetContactOrderID() const {
			return mContactOrderID;
		}

		void SetContactOrderID(int id) {
			mContactOrderID = id;
		}

		virtual void UpdateObject(float dt);

		bool GetIsPlayer() { return mIsPlayer; }

		CollisionLayer GetCollisionLayer() {
			return mCollisionLayer;
		}

		void SetCollisionLayer(CollisionLayer collisionLayer) {
			mCollisionLayer = collisionLayer;
		}

		GameObjectState GetGameOjbectState() {
			return mObjectState;
		}

		void SetName(const std::string& name) {
			mName = name;
		}

		void SetObjectState(GameObjectState state);

		void DrawCollisionVolume();

		const std::string& GetGameObjectStateStr() const;

		int GetServerID() const;
		void SetServerID(int serverID);
	protected:
		Transform			mTransform;

		CollisionVolume* mBoundingVolume;
		PhysicsObject* mPhysicsObject;
		RenderObject* mRenderObject;
		NetworkObject* mNetworkObject;
#ifndef DISTRIBUTEDSYSTEMACTIVE
		SoundObject* mSoundObject;
#endif

		bool		mIsSensed;
		bool		mHasPhysics;
		bool		mIsNetworkActive;
		bool		mIsHaloShadow = false;
		int			mControllerPlayerID = -1;
		Vector3		mMoveAxis;
		bool		mIsRendered;
		int			mWorldID;
		int			mContactOrderID = -1;
		std::string	mName;

		Vector3 mBroadphaseAABB;

		CollisionLayer mCollisionLayer;
		bool mIsPlayer;

		GameObjectState mObjectState;

		int mServerID = -1;
	};
}

