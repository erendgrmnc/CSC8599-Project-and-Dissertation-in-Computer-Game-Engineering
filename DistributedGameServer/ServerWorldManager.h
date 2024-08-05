#pragma once

namespace NCL::CSC8503 {
	struct StartSimulatingObjectPacket;
	class NetworkState;
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
			ServerWorldManager(int serverID, PhyscisServerBorderData& physcisServerBorderData, std::map<const int, PhyscisServerBorderData*>& map);
			NCL::CSC8503::GameWorld* GetGameWorld() const;

			CSC8503::GameObject* AddObjectToWorld(const CSC8503::Transform& transform);
			CSC8503::GameObject* AddFloorWorld(const CSC8503::Transform& transform);

			void Update(float dt);
			void AddNetworkObject(CSC8503::GameObject& objToAdd);
			void HandleIncomingObjectCreation(int networkObjectID);
			void StartHandlingObject(CSC8503::StartSimulatingObjectPacket* packet);
			void HandleOutgoingObject(int networkObjectID);

			std::vector<CSC8503::TestObject*> GetTestObjects();
			std::vector<CSC8503::NetworkObject*>* GetNetworkObjects();
		protected:
			int mNetworkIdBuffer;
			int mServerID;
			double mPhysicsTime = 0;

			std::vector<NCL::CSC8503::NetworkObject*> mNetworkObjects;
			std::vector<NCL::CSC8503::TestObject*> mTestObjects;

			NCL::CSC8503::GameWorld* mGameWorld;
			NCL::CSC8503::PhysicsSystem* mPhysics;
			NCL::DistributedGameServer::PhyscisServerBorderData* mServerBorderData;

			std::map<int, NCL::CSC8503::GameObject*> mCreatedObjectPool;
			std::map<const int, PhyscisServerBorderData*>* mServerBorderMap;

			void AddNetworkObjectToNetworkObjects(NCL::CSC8503::NetworkObject* networkObj);
			void CheckPredictedPositionOutOfServerBoundries();
			void CheckPositionOutOfServerBoundries();

			bool IsObjectInBorder(const Maths::Vector3& objectPosition) const;

			int GetObjectServer(const Maths::Vector3& position) const;

			const Maths::Vector3& CalculateIncomingObjectOffsetedPosition(const Maths::Vector3& position);
		};
	}
}
