#pragma once

namespace NCL::CSC8503 {
	class GameObject;
	class Transform;
	class NetworkObject;
	class PhysicsSystem;
	class GameWorld;
}

namespace NCL {
	namespace DistributedGameServer {
		class ServerWorldManager {
		public:
			ServerWorldManager();

			NCL::CSC8503::GameWorld* GetGameWorld() const;

			CSC8503::GameObject* AddObjectToWorld(const CSC8503::Transform& transform);
			CSC8503::GameObject* AddFloorWorld(const CSC8503::Transform& transform);

			void Update(float dt) const;
			void AddNetworkObject(CSC8503::GameObject& objToAdd);
		protected:
			int mNetworkIdBuffer;

			std::vector<NCL::CSC8503::NetworkObject*> mNetworkObjects;

			NCL::CSC8503::GameWorld* mGameWorld;
			NCL::CSC8503::PhysicsSystem* mPhysics;

			void AddNetworkObjectToNetworkObjects(NCL::CSC8503::NetworkObject* networkObj);
		};
	}
}
