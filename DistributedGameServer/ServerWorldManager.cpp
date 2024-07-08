#include "ServerWorldManager.h"

#include <fstream>

#include "GameWorld.h"
#include "NetworkObject.h"
#include "PhysicsObject.h"
#include "PhysicsSystem.h"
#include "TestObject.h"


namespace {
	constexpr int NETWORK_ID_BUFFER = 10;
}

NCL::DistributedGameServer::ServerWorldManager::ServerWorldManager(PhyscisServerBorderData& physcisServerBorderData) {
	mGameWorld = new NCL::CSC8503::GameWorld();

	mServerBorderData = &physcisServerBorderData;

	mPhysics = new PhysicsSystem(*mGameWorld);
	mPhysics->UseGravity(false);

	mNetworkIdBuffer = NETWORK_ID_BUFFER;

	Transform offsetKey = Transform();
	offsetKey.SetPosition(Vector3(0, 0, 0));
	if (IsObjectInBorder(offsetKey.GetPosition())) {
		AddFloorWorld(offsetKey);
	}
	
	Transform networkObjOffsetKey = Transform();
	offsetKey.SetPosition(Vector3(0, 10, 0));
	if (IsObjectInBorder(offsetKey.GetPosition())) {
		std::cout << "Adding network object.\n";
		auto* sphere = AddObjectToWorld(offsetKey);
		mTestObjects.push_back(static_cast<TestObject*>(sphere));
		AddNetworkObject(*sphere);
	}
}

NCL::CSC8503::GameWorld* NCL::DistributedGameServer::ServerWorldManager::GetGameWorld() const {
	return mGameWorld;
}

void NCL::DistributedGameServer::ServerWorldManager::Update(float dt) const {
	for (auto* testObject : mTestObjects) {
		testObject->Update(dt);
		std::cout << "ObjectPos: " << testObject->GetTransform().GetPosition() << "\n";
	}


	mGameWorld->UpdateWorld(dt);
	mPhysics->Update(dt);
}

void NCL::DistributedGameServer::ServerWorldManager::AddNetworkObject(CSC8503::GameObject& objToAdd) {
	auto* networkObj = new NetworkObject(objToAdd, mNetworkIdBuffer);
	mNetworkIdBuffer++;
	objToAdd.SetNetworkObject(networkObj);

	AddNetworkObjectToNetworkObjects(networkObj);
}

void NCL::DistributedGameServer::ServerWorldManager::AddNetworkObjectToNetworkObjects(CSC8503::NetworkObject* networkObj) {
	mNetworkObjects.push_back(networkObj);
}

bool DistributedGameServer::ServerWorldManager::IsObjectInBorder(const Maths::Vector3& objectPosition) {
	if (objectPosition.x >= mServerBorderData->minXVal && objectPosition.x < mServerBorderData->maxXVal &&
		objectPosition.z >= mServerBorderData->minZVal && objectPosition.z <= mServerBorderData->maxZVal) {
		return true;
	}
	return false;
}

NCL::CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::AddObjectToWorld(const Transform& transform) {
	TestObject* sphere = new TestObject();

	float radius = 1.f;
	Vector3 sphereSize = Vector3(radius, radius, radius);
	SphereVolume* volume = new SphereVolume(radius);
	sphere->SetBoundingVolume((CollisionVolume*)volume);

	sphere->GetTransform()
		.SetScale(sphereSize)
		.SetPosition(transform.GetPosition());

	sphere->SetPhysicsObject(new PhysicsObject(&sphere->GetTransform(), sphere->GetBoundingVolume()));

	sphere->GetPhysicsObject()->SetInverseMass(10.f);
	sphere->GetPhysicsObject()->InitSphereInertia(false);

	mGameWorld->AddGameObject(sphere);

	return (GameObject*)sphere;
}

NCL::CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::AddFloorWorld(const CSC8503::Transform& transform) {
	GameObject* floor = new GameObject();

	Vector3 floorSize = Vector3(100, 2, 100);
	AABBVolume* volume = new AABBVolume(floorSize);
	floor->SetBoundingVolume((CollisionVolume*)volume);

	floor->GetTransform()
		.SetScale(floorSize * 2)
		.SetPosition(transform.GetPosition());

	floor->SetPhysicsObject(new PhysicsObject(&floor->GetTransform(), floor->GetBoundingVolume()));

	floor->GetPhysicsObject()->SetInverseMass(0);
	floor->GetPhysicsObject()->InitCubeInertia();

	mGameWorld->AddGameObject(floor);

	return floor;
}
