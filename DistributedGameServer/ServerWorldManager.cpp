#include "ServerWorldManager.h"

#include <fstream>

#include "GameWorld.h"
#include "NetworkObject.h"
#include "PhysicsObject.h"
#include "PhysicsSystem.h"
#include "Profiler.h"
#include "TestObject.h"
#include "glad/gl.h"


namespace {
	constexpr int NETWORK_ID_BUFFER = 10;
	constexpr float PREDICTION_STEP = 1.0f;
}

NCL::DistributedGameServer::ServerWorldManager::ServerWorldManager(int serverID, PhyscisServerBorderData& physcisServerBorderData, std::map<const int, PhyscisServerBorderData*>& borderMap) {
	mServerID = serverID;

	mGameWorld = new NCL::CSC8503::GameWorld();

	mServerBorderData = &physcisServerBorderData;
	mServerBorderMap = &borderMap;

	mPhysics = new PhysicsSystem(*mGameWorld);
	mPhysics->Clear();
	mNetworkIdBuffer = NETWORK_ID_BUFFER;

	Transform offsetKey = Transform();
	offsetKey.SetPosition(Vector3(0, 0, 0));
	std::cout << "Added floor" << "\n";
	AddFloorWorld(offsetKey);

	offsetKey.SetPosition(Vector3(95, 50, 0));
	/*auto* sphere = AddDistributedControllableObject(offsetKey, 1);
	AddNetworkObject(*sphere);
	mCreatedObjectPool.insert(std::make_pair(sphere->GetNetworkObject()->GetnetworkID(), sphere));

	if (IsObjectInBorder(offsetKey.GetPosition())) {
		std::cout << "Adding object to world.\n";
		mTestObjects.push_back(dynamic_cast<TestObject*>(sphere));
	}
	else {
		std::cout << "Deactivating object because it is not in server borders. ID: " << sphere->GetNetworkObject()->GetnetworkID() << "\n";
		sphere->SetActive(false);
	}
	mGameWorld->AddGameObject(sphere);

	offsetKey.SetPosition(Vector3(-20, 50, 20));
	auto* sphereTwo = AddDistributedControllableObject(offsetKey, 2);
	AddNetworkObject(*sphereTwo);
	mCreatedObjectPool.insert(std::make_pair(sphereTwo->GetNetworkObject()->GetnetworkID(), sphereTwo));

	if (IsObjectInBorder(offsetKey.GetPosition())) {
		std::cout << "Adding object to world.\n";
		mTestObjects.push_back(dynamic_cast<TestObject*>(sphereTwo));
	}
	else {
		std::cout << "Deactivating object because it is not in server borders. ID: " << sphereTwo->GetNetworkObject()->GetnetworkID() << "\n";
		sphereTwo->SetActive(false);
	}
	mGameWorld->AddGameObject(sphereTwo);*/

	mPhysics->UseGravity(true);
}

NCL::CSC8503::GameWorld* NCL::DistributedGameServer::ServerWorldManager::GetGameWorld() const {
	return mGameWorld;
}

void NCL::DistributedGameServer::ServerWorldManager::Update(float dt) {
	std::chrono::steady_clock::time_point start;
	std::chrono::steady_clock::time_point end;
	std::chrono::duration<double, std::milli> timeTaken;

	mPhysicsTime = 0.f;
	mObjDebugTimer -= dt;
	int activeObjCount = 0;
	for (auto* testObject : mTestObjects) {
		if (testObject->HasPhysics()) {
			testObject->Update(dt);
			activeObjCount++;
			if (mObjDebugTimer <= 0.f) {
				std::cout << "Player position: " << testObject->GetTransform().GetPosition() << "\n";
				mObjDebugTimer = 5.f;
			}
		}
	}
	Profiler::SetObjectsInBorders(activeObjCount);

	start = std::chrono::high_resolution_clock::now();
	mGameWorld->UpdateWorld(dt);
	end = std::chrono::high_resolution_clock::now();
	timeTaken = end - start;
	Profiler::SetWorldTime(timeTaken.count());

	start = std::chrono::high_resolution_clock::now();
	mPhysics->PredictFuturePositions(dt);
	end = std::chrono::high_resolution_clock::now();
	timeTaken = end - start;
	Profiler::SetPhysicsPredictionTime(timeTaken.count());

	start = std::chrono::high_resolution_clock::now();
	mPhysics->Update(dt);
	end = std::chrono::high_resolution_clock::now();
	timeTaken = end - start;
	Profiler::SetPhysicsTime(timeTaken.count());

	CheckPositionOutOfServerBoundries();
}

void NCL::DistributedGameServer::ServerWorldManager::AddNetworkObject(CSC8503::GameObject& objToAdd) {
	std::cout << "Adding Network Object Id: " << mNetworkIdBuffer << "\n";
	auto* networkObj = new NetworkObject(objToAdd, mNetworkIdBuffer);
	mNetworkIdBuffer++;
	Profiler::SetTotalObjectsInServer(mNetworkIdBuffer - 10);
	objToAdd.SetNetworkObject(networkObj);

	AddNetworkObjectToNetworkObjects(networkObj);
}

void DistributedGameServer::ServerWorldManager::CreatePlayerObjects(int playerCount) {
	for (int i = 0; i < playerCount; i++) {
		Vector3 startPos;
		if (i == 0) {
			startPos.x = -50;
		}
		else if (i == 1) {
			startPos.x = 50;
		}

		startPos.y = 10;
		startPos.z = 0;

		CreateObjectGrid(10, 10, 1.f, 1.f, i + 1, startPos);
	}
}

void NCL::DistributedGameServer::ServerWorldManager::AddNetworkObjectToNetworkObjects(CSC8503::NetworkObject* networkObj) {
	mNetworkObjects.push_back(networkObj);
}


void DistributedGameServer::ServerWorldManager::CheckPositionOutOfServerBoundries() {
	for (const auto& gameObj : mGameWorld->GetGameObjects()) {
		if (gameObj->HasPhysics() && gameObj->IsNetworkActive()) {
			if (auto* networkComp = gameObj->GetNetworkObject()) {
				if (auto* physicsComp = gameObj->GetPhysicsObject()) {
					const Vector3& position = physicsComp->GetTransform()->GetPosition();
					int newServer = GetObjectServer(position);
					if (newServer != mServerID && newServer != -1) {

						networkComp->FinishTransitionToNewServer(newServer);
					}
				}
			}
		}
	}
}

bool DistributedGameServer::ServerWorldManager::StartHandlingObject(StartSimulatingObjectPacket* packet) {
	if (GameObject* objectToHandle = mCreatedObjectPool.at(packet->objectID)) {
		objectToHandle->SetActive(false);
		std::cout << "Added incoming network object with network id: " << packet->objectID << "/ Game world object count: " << mGameWorld->GetGameObjects().size() << "\n";

		if (TestObject* testComp = dynamic_cast<TestObject*>(objectToHandle)) {
			std::cout << "Added incoming player object \n";
			mTestObjects.push_back(testComp);
		}

		NetworkState& lastNetworkState = packet->lastFullState;
		std::cout << "Starting to simulate received object, received position: " << lastNetworkState.position << "\n";

		auto& transform = objectToHandle->GetTransform();

		objectToHandle->GetNetworkObject()->SetLatestNetworkState(lastNetworkState);

		transform.SetPosition(lastNetworkState.predictedPosition);
		transform.SetOrientation(lastNetworkState.predictedOrientation);

		transform.SetPredictedPosition(lastNetworkState.position);
		transform.SetPredictedOrientation(lastNetworkState.orientation);

		auto* physicsComp = objectToHandle->GetPhysicsObject();
		physicsComp->SetAngularVelocity(packet->mAngularVelocity);
		physicsComp->SetLinearVelocity(packet->mLinearVelocity);
		physicsComp->SetForce(packet->mForce);
		//physicsComp->SetTorque(packet->mTorque);

		//TODO(erendgrmnc): send handshake packet.


		objectToHandle->SetActive(true);
		objectToHandle->SetServerID(mServerID);

		std::cout << "Starting simulating object: " << packet->objectID << "/ Game world object count: " << mGameWorld->GetGameObjects().size() << "\n";
		return true;
	}

	return false;
}

void DistributedGameServer::ServerWorldManager::HandleTransitionHandshakeReceived(
	CSC8503::StartSimulatingObjectReceivedPacket* packet) {

}

void DistributedGameServer::ServerWorldManager::HandleOutgoingObject(int networkObjectID) {
	if (auto* gameObj = mCreatedObjectPool.at(networkObjectID)) {
		std::cout << "Removing object from server with network ID" << gameObj->GetNetworkObject()->GetnetworkID() << "\n";
		gameObj->SetActive(false);
		if (TestObject* testComp = dynamic_cast<TestObject*>(gameObj)) {
			std::erase(mTestObjects, testComp);
		}
	}
}

void DistributedGameServer::ServerWorldManager::CreateObjectGrid(int rowCount, int colCount, float rowSpacing,
	float colSpacing, int playerID, const Vector3& startPos) {
	int objCounter = 0;

	for (int x = 0; x < rowCount; ++x) {
		for (int z = 0; z < colCount; ++z) {
			Vector3 objPos = startPos;
			objPos.x += x * rowSpacing;
			objPos.z += z * colSpacing;

			Transform transform;
			transform.SetPosition(objPos);

			GameObject* obj = nullptr;

			if (rand() % 2) {
				std::cout << "Creating Object at: " << transform.GetPosition() << "\n";
				obj = AddCubeToWorld(transform, objCounter++, playerID);
			}
			else {
				obj = AddSphereToWorld(transform, objCounter++, playerID);
			}

			AddNetworkObject(*obj);
			auto networkId = obj->GetNetworkObject()->GetnetworkID();
			mCreatedObjectPool[networkId] = obj;

			if (IsObjectInBorder(transform.GetPosition())) {
				std::cout << "Added object to world. Obj name: " << obj->GetName() << "/ Network Id: " << networkId << "\n";
				mTestObjects.push_back(dynamic_cast<TestObject*>(obj));
			}
			else {
				obj->SetActive(false);
			}

			mGameWorld->AddGameObject(obj);
		}
	}
}

std::vector<CSC8503::TestObject*> DistributedGameServer::ServerWorldManager::GetTestObjects() {
	return mTestObjects;
}

std::vector<CSC8503::NetworkObject*>* DistributedGameServer::ServerWorldManager::GetNetworkObjects() {
	return &mNetworkObjects;
}

bool DistributedGameServer::ServerWorldManager::IsObjectInBorder(const Maths::Vector3& objectPosition) const {

	if (objectPosition.x >= mServerBorderData->minXVal && objectPosition.x < mServerBorderData->maxXVal &&
		objectPosition.z >= mServerBorderData->minZVal && objectPosition.z <= mServerBorderData->maxZVal) {
		return true;
	}

	return false;
}

int DistributedGameServer::ServerWorldManager::GetObjectServer(const Maths::Vector3& position) const {
	for (const auto& entry : *mServerBorderMap) {
		int serverNumber = entry.first;
		PhyscisServerBorderData* borderData = entry.second;

		if (position.x >= borderData->minXVal && position.x <= borderData->maxXVal &&
			position.z >= borderData->minZVal && position.z <= borderData->maxZVal) {

			return serverNumber;
		}
	}
	return -1;
}

const Maths::Vector3& DistributedGameServer::ServerWorldManager::CalculateIncomingObjectOffsetedPosition(const Maths::Vector3& position) {
	Vector3 offsetPos = position;

	if (position.x > mServerBorderData->maxXVal) {
		//offsetPos.x = std::floorf(position.x - 0.5f);
	}
	else {
		//offsetPos.x = std::ceilf(position.x + 0.5f);
	}

	if (position.z > mServerBorderData->maxZVal) {
		offsetPos.z = std::floor(position.z);
	}
	else if (position.z <= mServerBorderData->minZVal) {
		offsetPos.z = std::floor(position.z);
	}

	return offsetPos;
}

NCL::CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::AddDistributedControllableObject(const Transform& transform, int playerID) const {
	TestObject* sphere = new TestObject(*mServerBorderData, playerID);

	float radius = 5.f;
	Vector3 sphereSize = Vector3(radius, radius, radius);
	SphereVolume* volume = new SphereVolume(radius);
	sphere->SetBoundingVolume((CollisionVolume*)volume);

	sphere->GetTransform()
		.SetScale(sphereSize)
		.SetPosition(transform.GetPosition())
		.SetOrientation(transform.GetOrientation());

	sphere->SetPhysicsObject(new PhysicsObject(&sphere->GetTransform(), sphere->GetBoundingVolume()));

	sphere->GetPhysicsObject()->SetInverseMass(0.5f);
	sphere->GetPhysicsObject()->InitSphereInertia(false);
	sphere->SetName("Eren");

	sphere->SetCollisionLayer(Player);

	return (GameObject*)sphere;
}

CSC8503::GameObject* DistributedGameServer::ServerWorldManager::AddCubeToWorld(
	const CSC8503::Transform& transform, int count, int playerID) const {
	std::string objName = "Cube " + std::to_string(count);
	TestObject* cube = new TestObject(*mServerBorderData, playerID);
	cube->SetName(objName);

	float radius = 0.5f;
	Vector3 dimensions(radius, radius, radius);
	AABBVolume* volume = new AABBVolume(dimensions);
	cube->SetBoundingVolume((CollisionVolume*)volume);

	cube->GetTransform()
		.SetScale(dimensions * 2)
		.SetPosition(transform.GetPosition())
		.SetOrientation(transform.GetOrientation());

	cube->SetPhysicsObject(new PhysicsObject(&cube->GetTransform(), cube->GetBoundingVolume()));

	cube->GetPhysicsObject()->SetInverseMass(0.5f);
	cube->GetPhysicsObject()->InitSphereInertia(false);

	cube->SetCollisionLayer(CollisionLayer::NoSpecialFeatures);

	return cube;
}

CSC8503::GameObject* DistributedGameServer::ServerWorldManager::AddSphereToWorld(const CSC8503::Transform& transform,
	int count, int playerID) const {
	std::string objName = "Sphere " + std::to_string(count);
	GameObject* sphere = new TestObject(*mServerBorderData, playerID);

	float radius = 0.5f;
	Vector3 sphereSize = Vector3(radius, radius, radius);
	SphereVolume* volume = new SphereVolume(radius);
	sphere->SetBoundingVolume((CollisionVolume*)volume);

	sphere->GetTransform()
		.SetScale(sphereSize * 2)
		.SetPosition(transform.GetPosition())
		.SetOrientation(transform.GetOrientation());

	sphere->SetPhysicsObject(new PhysicsObject(&sphere->GetTransform(), sphere->GetBoundingVolume()));

	sphere->GetPhysicsObject()->SetInverseMass(0.5f);
	sphere->GetPhysicsObject()->InitSphereInertia(false);

	sphere->SetCollisionLayer(CollisionLayer::NoSpecialFeatures);

	return sphere;
}

NCL::CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::AddFloorWorld(const CSC8503::Transform& transform) {
	GameObject* floor = new GameObject(StaticObj, "Floor");

	Vector3 floorSize = Vector3(1000, 2, 1000);
	AABBVolume* volume = new AABBVolume(floorSize);
	floor->SetBoundingVolume((CollisionVolume*)volume);

	floor->GetTransform()
		.SetScale(floorSize * 1)
		.SetPosition(transform.GetPosition())
		.SetOrientation(transform.GetOrientation());

	floor->SetPhysicsObject(new PhysicsObject(&floor->GetTransform(), floor->GetBoundingVolume(), 0, 2, 2));

	floor->GetPhysicsObject()->SetInverseMass(0);
	floor->GetPhysicsObject()->InitCubeInertia();

	mGameWorld->AddGameObject(floor);

	return floor;
}
