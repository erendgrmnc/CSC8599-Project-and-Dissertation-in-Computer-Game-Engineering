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
	constexpr float PREDICTION_STEP = 0.1f;
}

NCL::DistributedGameServer::ServerWorldManager::ServerWorldManager(PhyscisServerBorderData& physcisServerBorderData) {
	mGameWorld = new NCL::CSC8503::GameWorld();

	mServerBorderData = &physcisServerBorderData;

	mPhysics = new PhysicsSystem(*mGameWorld);
	mPhysics->Clear();
	mNetworkIdBuffer = NETWORK_ID_BUFFER;

	Transform offsetKey = Transform();
	offsetKey.SetPosition(Vector3(0, 0, 0));
	if (true) {
		std::cout << "Added floor" << "\n";
		AddFloorWorld(offsetKey);
	}

	offsetKey.SetPosition(Vector3(0, 50, 0));
	if (IsObjectInBorder(offsetKey.GetPosition(), true)) {
		std::cout << "Adding network object.\n";
		auto* sphere = AddObjectToWorld(offsetKey);
		mTestObjects.push_back(static_cast<TestObject*>(sphere));
		AddNetworkObject(*sphere);
	}

	offsetKey.SetPosition(Vector3(-20, 50, 20));
	if (IsObjectInBorder(offsetKey.GetPosition(), true)) {
		std::cout << "Adding network object.\n";
		auto* sphereTwo = AddObjectToWorld(offsetKey);
		mTestObjects.push_back(static_cast<TestObject*>(sphereTwo));
		AddNetworkObject(*sphereTwo);
	}

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
		testObject->Update(dt);
	}
	start = std::chrono::high_resolution_clock::now();
	mGameWorld->UpdateWorld(dt);
	end = std::chrono::high_resolution_clock::now();
	worldUpdateTimeTaken = end - start;

	start = std::chrono::high_resolution_clock::now();
	mPhysics->Update(dt);

	mPhysics->PredictFuturePositions(PREDICTION_STEP);

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

bool DistributedGameServer::ServerWorldManager::IsObjectInBorder(const Maths::Vector3& objectPosition, bool isNetworkObject) {

	if (objectPosition.x >= mServerBorderData->minXVal && objectPosition.x < mServerBorderData->maxXVal &&
		objectPosition.z >= mServerBorderData->minZVal && objectPosition.z <= mServerBorderData->maxZVal) {
		return true;
	}
	if (isNetworkObject) {
		mNetworkIdBuffer++;
	}

	return false;
}

NCL::CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::AddObjectToWorld(const Transform& transform) {
	TestObject* sphere = new TestObject();

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
	mGameWorld->AddGameObject(sphere);

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
