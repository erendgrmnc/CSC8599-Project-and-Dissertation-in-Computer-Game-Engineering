#include "ServerWorldManager.h"

#include <fstream>

#include "Assets.h"
#include "GameWorld.h"
#include "NetworkObject.h"
#include "PhysicsObject.h"
#include "PhysicsSystem.h"

namespace {
	constexpr int NETWORK_ID_BUFFER = 10;
}

NCL::DistributedGameServer::ServerWorldManager::ServerWorldManager() {
	mGameWorld = new NCL::CSC8503::GameWorld();

	mPhysics = new PhysicsSystem(*mGameWorld);
	mPhysics->UseGravity(true);

	mNetworkIdBuffer = NETWORK_ID_BUFFER;

	Transform offsetKey = Transform();
	offsetKey.SetPosition(Vector3(0, 0, 0));
	AddFloorWorld(offsetKey);

	Transform networkObjOffsetKey = Transform();
	offsetKey.SetPosition(Vector3(0, 10, 0));
	auto* sphere = AddObjectToWorld(offsetKey);
	AddNetworkObject(*sphere);
}

NCL::CSC8503::GameWorld* NCL::DistributedGameServer::ServerWorldManager::GetGameWorld() const {
	return mGameWorld;
}

void NCL::DistributedGameServer::ServerWorldManager::Update(float dt) const {
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

NCL::CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::AddObjectToWorld(const Transform& transform) {
	GameObject* sphere = new GameObject();

	float radius = 1.f;
	Vector3 sphereSize = Vector3(radius, radius, radius);
	SphereVolume* volume = new SphereVolume(radius);
	sphere->SetBoundingVolume((CollisionVolume*)volume);

	sphere->GetTransform()
		.SetScale(sphereSize)
		.SetPosition(transform.GetPosition());

	sphere->SetPhysicsObject(new PhysicsObject(&sphere->GetTransform(), sphere->GetBoundingVolume()));

	sphere->GetPhysicsObject()->SetInverseMass(0);
	sphere->GetPhysicsObject()->InitSphereInertia(false);

	mGameWorld->AddGameObject(sphere);

	return sphere;
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
