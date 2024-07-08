#pragma once

namespace NCL::CSC8503 {
	class TestObject;
	class GameObject;
	class Transform;
	class NetworkObject;
	class PhysicsSystem;
	class GameWorld;
}

namespace NCL {
	namespace DistributedGameServer {

		struct PhyscisServerBorderData {
			int maxZVal;
			int minZVal;

			int maxXVal;
			int minXVal;
		};

		class ServerWorldManager {
		public:
			ServerWorldManager(PhyscisServerBorderData& physcisServerBorderData);

			NCL::CSC8503::GameWorld* GetGameWorld() const;

			CSC8503::GameObject* AddObjectToWorld(const CSC8503::Transform& transform);
			CSC8503::GameObject* AddFloorWorld(const CSC8503::Transform& transform);

			void Update(float dt) const;
			void AddNetworkObject(CSC8503::GameObject& objToAdd);
		protected:
			int mNetworkIdBuffer;

			std::vector<NCL::CSC8503::NetworkObject*> mNetworkObjects;
			std::vector<NCL::CSC8503::TestObject*> mTestObjects;

			NCL::CSC8503::GameWorld* mGameWorld;
			NCL::CSC8503::PhysicsSystem* mPhysics;
			NCL::DistributedGameServer::PhyscisServerBorderData* mServerBorderData;

			void AddNetworkObjectToNetworkObjects(NCL::CSC8503::NetworkObject* networkObj);
			bool IsObjectInBorder(const Maths::Vector3& objectPosition);
		};
	}
}
