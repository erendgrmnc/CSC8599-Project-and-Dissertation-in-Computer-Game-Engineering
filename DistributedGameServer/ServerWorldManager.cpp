#include "ServerWorldManager.h"

#include <fstream>

#include "GameWorld.h"
#include "NetworkObject.h"
#include "PhysicsObject.h"
#include "PhysicsSystem.h"
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
	auto* sphere = AddObjectToWorld(offsetKey);
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

	offsetKey.SetPosition(Vector3(-95, 50, 20));
	/*auto* sphereTwo = AddObjectToWorld(offsetKey);
	AddNetworkObject(*sphereTwo);
	mCreatedObjectPool.insert(std::make_pair(sphereTwo->GetNetworkObject()->GetnetworkID(), sphereTwo));
	if (IsObjectInBorder(offsetKey.GetPosition())) {
		std::cout << "Adding object to world.\n";
		mGameWorld->AddGameObject(sphereTwo);
		mTestObjects.push_back(dynamic_cast<TestObject*>(sphereTwo));
	}*/

	mPhysics->UseGravity(true);
}

NCL::CSC8503::GameWorld* NCL::DistributedGameServer::ServerWorldManager::GetGameWorld() const {
	return mGameWorld;
}

void NCL::DistributedGameServer::ServerWorldManager::Update(float dt) {
	std::chrono::steady_clock::time_point start;
	std::chrono::steady_clock::time_point end;
	std::chrono::duration<double, std::milli> timeTaken;
	std::chrono::duration<double, std::milli> worldUpdateTimeTaken;

	mPhysicsTime = 0.f;
	for (auto* testObject : mTestObjects) {
		if (testObject->HasPhysics()) {
			testObject->Update(dt);
		}
	}
	start = std::chrono::high_resolution_clock::now();
	mGameWorld->UpdateWorld(dt);
	end = std::chrono::high_resolution_clock::now();
	worldUpdateTimeTaken = end - start;

	start = std::chrono::high_resolution_clock::now();

	mPhysics->PredictFuturePositions(dt);
	mPhysics->Update(dt);
	CheckPredictedPositionOutOfServerBoundries();
	CheckPositionOutOfServerBoundries();

	end = std::chrono::high_resolution_clock::now();
	timeTaken = end - start;
	mPhysicsTime = timeTaken.count();

	//std::cout << "World Update Time: " << worldUpdateTimeTaken << "\n";
	//std::cout << "Physics Time: " << timeTaken << "\n";
}

void NCL::DistributedGameServer::ServerWorldManager::AddNetworkObject(CSC8503::GameObject& objToAdd) {
	std::cout << "Adding Network Object Id: " << mNetworkIdBuffer << "\n";
	auto* networkObj = new NetworkObject(objToAdd, mNetworkIdBuffer);
	mNetworkIdBuffer++;
	objToAdd.SetNetworkObject(networkObj);

	AddNetworkObjectToNetworkObjects(networkObj);
}

void NCL::DistributedGameServer::ServerWorldManager::AddNetworkObjectToNetworkObjects(CSC8503::NetworkObject* networkObj) {
	mNetworkObjects.push_back(networkObj);
}

void DistributedGameServer::ServerWorldManager::CheckPredictedPositionOutOfServerBoundries() {
	for (const auto& gameObj : mGameWorld->GetGameObjects()) {
		if (gameObj->HasPhysics() && gameObj->IsNetworkActive()) {
			if (auto* networkComp = gameObj->GetNetworkObject()) {
				if (!networkComp->GetIsPredictionInfoSent()) {
					if (auto* physicsComp = gameObj->GetPhysicsObject()) {
						const Vector3& predictedPos = physicsComp->GetPredictedPosition();
						int objectServer = GetObjectServer(predictedPos);
						if (objectServer != mServerID && objectServer != -1 && !networkComp->GetIsOnTransitionCooldown()) {
							networkComp->StartTransitionToNewServer(objectServer);
						}
					}
				}
			}
		}
	}
}

void DistributedGameServer::ServerWorldManager::CheckPositionOutOfServerBoundries() {
	for (const auto& gameObj : mGameWorld->GetGameObjects()) {
		if (gameObj->HasPhysics() && gameObj->IsNetworkActive()) {
			if (auto* networkComp = gameObj->GetNetworkObject()) {
				if (auto* physicsComp = gameObj->GetPhysicsObject()) {
					const Vector3& position = physicsComp->GetTransform()->GetPosition();
					int newServer = GetObjectServer(position);
					if (newServer != mServerID && newServer != -1 && !networkComp->GetIsOnTransitionCooldown()) {
						networkComp->FinishTransitionToNewServer();
					}
				}
			}
		}
	}
}

void DistributedGameServer::ServerWorldManager::HandleIncomingObjectCreation(int networkObjectID) {
	GameObject* objectToHandle = mCreatedObjectPool.at(networkObjectID);
	objectToHandle->SetActive(false);
	//mGameWorld->AddGameObject(objectToHandle);
	//objectToHandle->GetNetworkObject()->HandleReceiveFromAnotherServer();

	std::cout << "Added incoming network object with network id: " << networkObjectID << "/ Game world object count: " << mGameWorld->GetGameObjects().size() << "\n";

	//For testing
	if (TestObject* testComp = dynamic_cast<TestObject*>(objectToHandle)) {
		mTestObjects.push_back(testComp);
	}
}

void DistributedGameServer::ServerWorldManager::StartHandlingObject(StartSimulatingObjectPacket* packet) {

	NetworkState& lastNetworkState = packet->lastFullState;
	std::cout << "Starting to simulate received object, received position: " << lastNetworkState.position << "\n";
	GameObject* objectToHandle = mCreatedObjectPool.at(packet->objectID);

	auto& transform = objectToHandle->GetTransform();

	objectToHandle->GetNetworkObject()->SetLatestNetworkState(lastNetworkState);

	transform.SetPosition(lastNetworkState.position);
	transform.SetOrientation(lastNetworkState.orientation);

	auto* physicsComp = objectToHandle->GetPhysicsObject();
	physicsComp->SetPredictedPosition(lastNetworkState.position);
	physicsComp->SetPredictedOrientation(lastNetworkState.orientation);
	physicsComp->SetAngularVelocity(packet->mAngularVelocity);
	physicsComp->SetForce(packet->mForce);

	//TODO(erendgrmnc): Handle physics properties.

	objectToHandle->SetActive(true);
	objectToHandle->SetServerID(mServerID);

	std::cout << "Starting simulating object: " << packet->objectID << "/ Game world object count: " << mGameWorld->GetGameObjects().size() << "\n";
}

void DistributedGameServer::ServerWorldManager::HandleOutgoingObject(int networkObjectID) {
	if (auto* gameObj = mCreatedObjectPool.at(networkObjectID)) {
		std::cout << "Removing object from server with network ID" << gameObj->GetNetworkObject()->GetnetworkID() << "\n";
		//mGameWorld->RemoveGameObject(gameObj);
		gameObj->SetActive(false);
		if (TestObject* testComp = dynamic_cast<TestObject*>(gameObj)) {
			std::erase(mTestObjects, testComp);
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
	else if(position.z <= mServerBorderData->minZVal) {
		offsetPos.z = std::floor(position.z);
	}

	return offsetPos;
}

NCL::CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::AddObjectToWorld(const Transform& transform) {
	TestObject* sphere = new TestObject(*mServerBorderData, 1);

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
