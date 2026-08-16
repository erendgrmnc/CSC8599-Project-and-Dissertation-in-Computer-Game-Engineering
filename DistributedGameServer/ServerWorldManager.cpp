#include "ServerWorldManager.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "GameWorld.h"
#include "NetworkObject.h"
#include "PhysicsObject.h"
#include "PhysicsSystem.h"
#include "Profiler.h"
#include "TestObject.h"
#include "glad/gl.h"

using namespace NCL;


namespace {
	constexpr int NETWORK_ID_BUFFER = 10;
	constexpr float PREDICTION_STEP = 1.0f;

	// "shuttle" workload tuning. Linear damping is (1 - 0.4*dt) per substep, i.e. a
	// velocity time-constant of ~2.5 s, so an object launched at V travels roughly
	// 2.5*V before stopping. At 30-60 u/s that is 75-150 units of travel, which
	// comfortably crosses a border in a +/-150 world.
	constexpr float SHUTTLE_MIN_SPEED = 30.0f;
	constexpr float SHUTTLE_MAX_SPEED = 60.0f;
	constexpr float SHUTTLE_Z_SPREAD = 20.0f;

	// Objects are radius 0.5, i.e. one unit across. The grid spacing used to be 1.0,
	// so they spawned exactly touching and the opening seconds of every run were
	// dominated by resolving the initial contacts rather than by the workload.
	constexpr float OBJECT_GRID_SPACING = 2.0f;

	// Where each player's grid is centred, alternating either side of the origin.
	constexpr float PLAYER_START_OFFSET = 50.0f;
	constexpr float PLAYER_START_STRIDE = 40.0f;

	// Deterministic replacement for rand() when choosing an object's shape.
	//
	// Every server independently builds the identical object set, and agreement
	// used to rest on rand() being unseeded (no srand call exists anywhere), so all
	// processes happened to walk libc's default sequence in lockstep. That breaks the
	// moment any process seeds the generator, calls rand() elsewhere, or builds the
	// grid in a different order.
	//
	// Hashing the object's identity instead of drawing from a shared stream makes the
	// choice independent of call order as well as reproducible across processes.
	unsigned int DeterministicHash(unsigned int seed, int playerID, int objectIndex) {
		unsigned int h = seed * 2654435761u;
		h ^= static_cast<unsigned int>(playerID) + 0x9e3779b9u + (h << 6) + (h >> 2);
		h ^= static_cast<unsigned int>(objectIndex) + 0x9e3779b9u + (h << 6) + (h >> 2);
		h ^= h >> 16;
		h *= 0x7feb352du;
		h ^= h >> 15;
		return h;
	}
}

NCL::DistributedGameServer::ServerWorldManager::ServerWorldManager(int serverID, PhysicsServerBorderData& physcisServerBorderData, std::map<const int, PhysicsServerBorderData*>& borderMap) {
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
	mCreatedObjectPool.insert(std::make_pair(sphere->GetNetworkObject()->GetNetworkID(), sphere));

	if (IsObjectInBorder(offsetKey.GetPosition())) {
		std::cout << "Adding object to world.\n";
		mTestObjects.push_back(dynamic_cast<TestObject*>(sphere));
	}
	else {
		std::cout << "Deactivating object because it is not in server borders. ID: " << sphere->GetNetworkObject()->GetNetworkID() << "\n";
		sphere->SetActive(false);
	}
	mGameWorld->AddGameObject(sphere);

	offsetKey.SetPosition(Vector3(-20, 50, 20));
	auto* sphereTwo = AddDistributedControllableObject(offsetKey, 2);
	AddNetworkObject(*sphereTwo);
	mCreatedObjectPool.insert(std::make_pair(sphereTwo->GetNetworkObject()->GetNetworkID(), sphereTwo));

	if (IsObjectInBorder(offsetKey.GetPosition())) {
		std::cout << "Adding object to world.\n";
		mTestObjects.push_back(dynamic_cast<TestObject*>(sphereTwo));
	}
	else {
		std::cout << "Deactivating object because it is not in server borders. ID: " << sphereTwo->GetNetworkObject()->GetNetworkID() << "\n";
		sphereTwo->SetActive(false);
	}
	mGameWorld->AddGameObject(sphereTwo);*/

	mPhysics->UseGravity(true);
}

NCL::CSC8503::GameWorld* NCL::DistributedGameServer::ServerWorldManager::GetGameWorld() const {
	return mGameWorld;
}

void NCL::DistributedGameServer::ServerWorldManager::SetFixedTimestep(bool state) {
	mPhysics->SetFixedTimestep(state);
}

void NCL::DistributedGameServer::ServerWorldManager::EnableMetrics(const std::string& outputPath, size_t capacity) {
	mMetrics = std::make_unique<NCL::MetricSink>(outputPath, capacity);
	std::cout << "Per-tick metrics -> " << outputPath << " (capacity " << capacity << " samples)\n";
}

void NCL::DistributedGameServer::ServerWorldManager::FlushMetrics() {
	if (mMetrics) {
		mMetrics->Flush();
	}
}

// Gives a freshly created object its initial motion. Without a workload the default
// scene is purely ballistic - objects fall straight down and settle - so nothing ever
// approaches a region border and the handoff protocol, which is the whole point of
// the system, is never exercised.
//
// The velocity is derived from the same deterministic hash used for shape selection,
// so every server computes the identical value for a given object without any
// coordination, and a run repeats exactly for a given --seed.
void NCL::DistributedGameServer::ServerWorldManager::ApplyWorkloadInitialState(
	CSC8503::GameObject& obj, int playerID, int objectIndex) const {
	if (mWorkload != "shuttle") {
		return;
	}

	auto* physicsComp = obj.GetPhysicsObject();
	if (physicsComp == nullptr) {
		return;
	}

	const unsigned int h = DeterministicHash(mWorldSeed ^ 0xA5A5A5A5u, playerID, objectIndex);

	// Lateral speed in [SHUTTLE_MIN_SPEED, SHUTTLE_MAX_SPEED], direction alternating
	// by hash so traffic crosses the border in both directions rather than draining
	// into one region.
	const float span = SHUTTLE_MAX_SPEED - SHUTTLE_MIN_SPEED;
	const float speed = SHUTTLE_MIN_SPEED + (static_cast<float>(h % 1000u) / 1000.0f) * span;
	const float direction = (h & 1u) ? 1.0f : -1.0f;

	// A small Z component spreads objects along the border instead of funnelling them
	// through a single crossing point.
	const float lateralZ = (static_cast<float>((h >> 8) % 200u) / 200.0f - 0.5f) * SHUTTLE_Z_SPREAD;

	physicsComp->SetLinearVelocity(Vector3(speed * direction, 0.0f, lateralZ));
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

	CheckPositionOutOfServerBoundaries();

	Profiler::SetHandoffsSent(mHandoffsSent);
	Profiler::SetHandoffsReceived(mHandoffsReceived);
	Profiler::SetHandoffsFailed(mHandoffsFailed);

	// Per-tick record. The @@STAT line above is a 2 Hz instantaneous sample and
	// cannot describe a distribution; this is what the paper's timing figures are
	// built from. Recording is a push_back into a pre-reserved buffer - no
	// allocation, no I/O, nothing that would perturb what is being measured.
	if (mMetrics) {
		NCL::TickSample sample;
		sample.tick = mTickCounter;
		sample.timeMicros = NCL::MonotonicMicros();
		sample.physicsMs = static_cast<float>(Profiler::GetPhysicsTime());
		sample.predictMs = static_cast<float>(Profiler::GetPhysicsPredictionTime());
		sample.worldMs = static_cast<float>(Profiler::GetWorldTime());
		sample.ownedObjects = activeObjCount;
		sample.integratedObjects = Profiler::GetIntegratedObjects();
		sample.handoffsSent = mHandoffsSent;
		sample.handoffsReceived = mHandoffsReceived;
		sample.handoffsFailed = mHandoffsFailed;
		mMetrics->Record(sample);
	}
	++mTickCounter;
}

void NCL::DistributedGameServer::ServerWorldManager::AddNetworkObject(CSC8503::GameObject& objToAdd) {
	std::cout << "Adding Network Object Id: " << mNetworkIdBuffer << "\n";
	auto* networkObj = new NetworkObject(objToAdd, mNetworkIdBuffer);
	mNetworkIdBuffer++;
	Profiler::SetTotalObjectsInServer(mNetworkIdBuffer - 10);
	objToAdd.SetNetworkObject(networkObj);

	AddNetworkObjectToNetworkObjects(networkObj);
}

void DistributedGameServer::ServerWorldManager::CreatePlayerObjects(int playerCount, int objectsPerPlayer) {
	for (int i = 0; i < playerCount; i++) {
		Vector3 startPos;

		// Players alternate either side of the origin, stepping outwards. Previously
		// only players 0 and 1 were positioned at all, so every player from index 2
		// spawned its whole grid at x=0, stacked inside the others.
		const int pairIndex = i / 2;
		const float side = (i % 2 == 0) ? -1.0f : 1.0f;
		startPos.x = side * (PLAYER_START_OFFSET + pairIndex * PLAYER_START_STRIDE);
		startPos.y = 10.f;
		startPos.z = 0.f;

		// Grid sized to the requested object count. This was hardcoded 10x10, which
		// silently capped every player at 100 objects however many were asked for -
		// making an object-count sweep impossible and reporting nothing.
		const int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(objectsPerPlayer)))));
		const int rows = std::max(1, static_cast<int>(std::ceil(static_cast<double>(objectsPerPlayer) / cols)));

		CreateObjectGrid(rows, cols, objectsPerPlayer, OBJECT_GRID_SPACING, OBJECT_GRID_SPACING, i, startPos);
	}
}

void NCL::DistributedGameServer::ServerWorldManager::AddNetworkObjectToNetworkObjects(CSC8503::NetworkObject* networkObj) {
	mNetworkObjects.push_back(networkObj);
}


void DistributedGameServer::ServerWorldManager::CheckPositionOutOfServerBoundaries() {
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
	// std::map::at throws on an unknown key; an object present on the sender but
	// absent from this server's pool would take the whole process down rather than
	// reporting a failed handoff.
	auto poolEntry = mCreatedObjectPool.find(packet->objectID);
	if (poolEntry == mCreatedObjectPool.end()) {
		++mHandoffsFailed;
		std::cout << "ERROR: handoff for unknown object id " << packet->objectID
			<< " - no pool entry on this server.\n";
		return false;
	}
	if (GameObject* objectToHandle = poolEntry->second) {
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

		++mHandoffsReceived;

		std::cout << "Starting simulating object: " << packet->objectID << "/ Game world object count: " << mGameWorld->GetGameObjects().size() << "\n";
		return true;
	}

	++mHandoffsFailed;
	return false;
}

void DistributedGameServer::ServerWorldManager::HandleTransitionHandshakeReceived(
	CSC8503::StartSimulatingObjectReceivedPacket* packet) {

}

void DistributedGameServer::ServerWorldManager::HandleOutgoingObject(int networkObjectID) {
	auto poolEntry = mCreatedObjectPool.find(networkObjectID);
	if (poolEntry == mCreatedObjectPool.end()) {
		std::cout << "ERROR: outgoing handoff for unknown object id " << networkObjectID << "\n";
		return;
	}
	if (auto* gameObj = poolEntry->second) {
		std::cout << "Removing object from server with network ID" << gameObj->GetNetworkObject()->GetNetworkID() << "\n";
		gameObj->SetActive(false);
		if (TestObject* testComp = dynamic_cast<TestObject*>(gameObj)) {
			std::erase(mTestObjects, testComp);
		}
	}
}

void DistributedGameServer::ServerWorldManager::CreateObjectGrid(int rowCount, int colCount, int objectsPerPlayer, float rowSpacing,
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

			if (DeterministicHash(mWorldSeed, playerID, objCounter) & 1u) {
				std::cout << "Creating Object at: " << transform.GetPosition() << "\n";
				obj = AddCubeToWorld(transform, objCounter++, playerID);
			}
			else {
				obj = AddSphereToWorld(transform, objCounter++, playerID);
			}

			AddNetworkObject(*obj);
			auto networkId = obj->GetNetworkObject()->GetNetworkID();
			mCreatedObjectPool[networkId] = obj;

			ApplyWorkloadInitialState(*obj, playerID, objCounter - 1);

			if (IsObjectInBorder(transform.GetPosition())) {
				std::cout << "Added object to world. Obj name: " << obj->GetName() << "/ Network Id: " << networkId << "\n";
				mTestObjects.push_back(dynamic_cast<TestObject*>(obj));
			}
			else {
				obj->SetActive(false);
			}

			mGameWorld->AddGameObject(obj);

			if (objectsPerPlayer == objCounter) {
				return;
			}
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
		PhysicsServerBorderData* borderData = entry.second;

		if (position.x >= borderData->minXVal && position.x <= borderData->maxXVal &&
			position.z >= borderData->minZVal && position.z <= borderData->maxZVal) {

			return serverNumber;
		}
	}
	return -1;
}

// Clamps a handed-over object's position strictly inside this server's region.
//
// An object arriving from a neighbour can land exactly on, or a hair past, the
// shared edge: the sender decided the object had left its own region, but float
// rounding can leave the position on a coordinate this server's border test also
// rejects. Both servers then disown it. This nudge makes the receiver's test
// agree with the handoff that just happened.
//
// The bounds mirror IsObjectInBorder exactly - half-open on X (>= min, < max),
// closed on Z (>= min, <= max) - so a position this returns always satisfies it.
// When the incoming position is already inside, every clamp is a no-op.
//
// NOTE: still unreferenced. Wiring it into StartHandlingObject moves incoming
// objects and so changes measured handoff behaviour; that belongs with the
// ownership unification (increment 2 of the interactions design), not here.
Maths::Vector3 DistributedGameServer::ServerWorldManager::CalculateIncomingObjectOffsetPosition(const Maths::Vector3& position) const {
	// One centimetre in world units - large enough to survive the float rounding
	// that put the object on the edge, far below the 2-unit object spacing.
	constexpr float INWARD_EPSILON = 0.01f;

	Vector3 offsetPos = position;

	// X's upper bound is exclusive, so max itself is not a legal position here.
	// std::clamp is undefined when lo > hi, which a degenerate region would cause.
	const float highX = std::max(mServerBorderData->minXVal, mServerBorderData->maxXVal - INWARD_EPSILON);
	offsetPos.x = std::clamp(position.x, mServerBorderData->minXVal, highX);

	// Z's upper bound is inclusive, so max is legal and needs no epsilon.
	const float highZ = std::max(mServerBorderData->minZVal, mServerBorderData->maxZVal);
	offsetPos.z = std::clamp(position.z, mServerBorderData->minZVal, highZ);

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
