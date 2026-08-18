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

	// Force applied per unit of movement-axis input. Tuned against the shuttle
	// workload's 30-60 u/s so a driven object is comparable to a launched one.
	constexpr float MOVE_AXIS_FORCE = 200.0f;

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

// Defined here, where StartSimulatingObjectPacket is complete, so the scheduled
// handoff buffer's unique_ptr deleter can be instantiated.
NCL::DistributedGameServer::ServerWorldManager::~ServerWorldManager() = default;

NCL::CSC8503::GameWorld* NCL::DistributedGameServer::ServerWorldManager::GetGameWorld() const {
	return mGameWorld;
}

// Out of line: GameWorld is only forward-declared in the header.
int NCL::DistributedGameServer::ServerWorldManager::GetWorldObjectCount() const {
	return mGameWorld ? static_cast<int>(mGameWorld->GetGameObjects().size()) : 0;
}

void NCL::DistributedGameServer::ServerWorldManager::SetFixedTimestep(bool state) {
	mPhysics->SetFixedTimestep(state);
}

float NCL::DistributedGameServer::ServerWorldManager::GetFixedTimestepDt() const {
	const int hz = mPhysics->GetSubstepHZ();
	return (hz > 0) ? (1.0f / static_cast<float>(hz)) : 0.0f;
}

// ---------------------------------------------------------------------------
// ICommandContext
// ---------------------------------------------------------------------------

int NCL::DistributedGameServer::ServerWorldManager::GetServerID() const {
	return mServerID;
}

int NCL::DistributedGameServer::ServerWorldManager::GetOwningServer(const Maths::Vector3& worldPoint) const {
	// Deliberately delegates rather than re-deriving: GetObjectServer is the single
	// place the region test lives, so commands and handoff cannot disagree.
	return GetObjectServer(worldPoint);
}

NCL::CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::FindActiveObject(int networkObjectID) const {
	const auto entry = mCreatedObjectPool.find(networkObjectID);
	if (entry == mCreatedObjectPool.end() || entry->second == nullptr) {
		return nullptr;
	}
	// Every server holds a pool entry for every object; only the owner has it active.
	// That is exactly the ownership test a command needs.
	if (!entry->second->IsNetworkActive()) {
		return nullptr;
	}
	return entry->second;
}

bool NCL::DistributedGameServer::ServerWorldManager::TryGetLastKnownPosition(int networkObjectID,
	Maths::Vector3& out) const {
	const auto entry = mCreatedObjectPool.find(networkObjectID);
	if (entry == mCreatedObjectPool.end() || entry->second == nullptr) {
		return false;
	}
	out = entry->second->GetTransform().GetPosition();
	return true;
}

NCL::CSC8503::GameObject* NCL::DistributedGameServer::ServerWorldManager::CreateObjectFromArchetype(
	int archetypeID, const Maths::Vector3& position, int networkID, int playerID) {
	Transform transform;
	transform.SetPosition(position);

	// The archetype is chosen explicitly rather than by a hash of the seed, so every
	// server and every client builds the same shape for the same id.
	GameObject* object = (archetypeID == static_cast<int>(NCL::Interaction::ObjectArchetype::Sphere))
		? AddSphereToWorld(transform, networkID, playerID)
		: AddCubeToWorld(transform, networkID, playerID);

	// An explicit id, not the pre-seed counter: runtime ids come from the partitioned
	// space and must be identical on every server.
	auto* networkObject = new NetworkObject(*object, networkID);
	object->SetNetworkObject(networkObject);
	AddNetworkObjectToNetworkObjects(networkObject);

	mCreatedObjectPool[networkID] = object;
	mObjectArchetypes[networkID] = archetypeID;
	mGameWorld->AddGameObject(object);

	// Without this the object is invisible to the integrator AND to
	// PredictFuturePositions, so it would neither fall nor ever be handed off.
	mPhysics->RegisterObject(object);

	return object;
}

int NCL::DistributedGameServer::ServerWorldManager::SpawnObject(int archetypeID,
	const Maths::Vector3& at, int spawnerPlayerID) {
	const int networkID = NCL::NetworkIdSpace::MakeRuntimeId(mServerID, mRuntimeSpawnCounter);
	if (networkID < 0) {
		std::cout << "ERROR: runtime id space exhausted on server " << mServerID
			<< " after " << mRuntimeSpawnCounter << " spawns.\n";
		return -1;
	}
	++mRuntimeSpawnCounter;

	GameObject* object = CreateObjectFromArchetype(archetypeID, at, networkID, spawnerPlayerID);
	if (object == nullptr) {
		return -1;
	}

	// The owner activates; peers will hold a deactivated twin. This is the pre-seed
	// model reproduced at runtime, which is what lets handoff work unchanged.
	const bool ownedHere = (GetObjectServer(at) == mServerID);
	object->SetActive(ownedHere);
	if (ownedHere) {
		if (auto* testObject = dynamic_cast<TestObject*>(object)) {
			mTestObjects.push_back(testObject);
		}

		// Under the shuttle workload a spawned object gets the same lateral motion a
		// pre-seeded one does. Without it a spawn just falls and settles where it
		// landed, so it would never cross a border - and the whole point of giving
		// peers a deactivated twin is that a runtime object CAN be handed off.
		if (mWorkload == "shuttle" && object->GetPhysicsObject() != nullptr) {
			const unsigned int hash = DeterministicHash(mWorldSeed, mServerID, mRuntimeSpawnCounter);
			const float speed = SHUTTLE_MIN_SPEED +
				static_cast<float>(hash % 1000u) * 0.001f * (SHUTTLE_MAX_SPEED - SHUTTLE_MIN_SPEED);
			// Aim across the nearest border rather than outward, so the handoff path
			// is exercised rather than the world edge.
			const float direction = (at.x < 0.0f) ? 1.0f : -1.0f;
			object->GetPhysicsObject()->SetLinearVelocity(Vector3(speed * direction, 0.0f, 0.0f));
		}
	}

	PendingSpawn pending;
	pending.objectID = networkID;
	pending.archetypeID = archetypeID;
	pending.ownerServerID = mServerID;
	pending.spawnerPlayerID = spawnerPlayerID;
	pending.position = at;
	mPendingSpawns.push_back(pending);

	return networkID;
}

bool NCL::DistributedGameServer::ServerWorldManager::PopPendingSpawn(PendingSpawn& out) {
	if (mPendingSpawns.empty()) {
		return false;
	}
	out = mPendingSpawns.front();
	mPendingSpawns.erase(mPendingSpawns.begin());
	return true;
}

bool NCL::DistributedGameServer::ServerWorldManager::CreateReplicatedSpawn(int networkID,
	int archetypeID, int ownerServerID, int spawnerPlayerID, const Maths::Vector3& position) {
	if (mCreatedObjectPool.find(networkID) != mCreatedObjectPool.end()) {
		return false;   // Already known; a duplicate broadcast is not an error.
	}

	GameObject* object = CreateObjectFromArchetype(archetypeID, position, networkID, spawnerPlayerID);
	if (object == nullptr) {
		return false;
	}

	// Deactivated: this server holds the twin so a future handoff can reactivate it,
	// exactly as it would for a pre-seeded object it does not currently own.
	object->SetActive(false);
	std::cout << "Created deactivated twin for runtime object " << networkID
		<< " owned by server " << ownerServerID << "\n";
	return true;
}

// Tears an object down locally. Deliberately does NOT delete it: mDynamicObjectList,
// mStaticTree and the collision sets all hold raw GameObject*, and UpdateCollisionList
// dereferences them for up to mNumCollisionFrames frames after a contact ends.
void NCL::DistributedGameServer::ServerWorldManager::TeardownObject(CSC8503::GameObject* object) {
	if (object == nullptr) {
		return;
	}

	object->SetActive(false);
	// Defers the structural removal and purges the collision sets (increment 1).
	mPhysics->UnregisterObject(object);
	// andDelete stays false: this object is still referenced by the physics system
	// until the next flush.
	mGameWorld->RemoveGameObject(object, false);

	mTestObjects.erase(
		std::remove(mTestObjects.begin(), mTestObjects.end(), object),
		mTestObjects.end());

	if (auto* networkObject = object->GetNetworkObject()) {
		std::erase(mNetworkObjects, networkObject);
	}

	mPendingDeletion.push_back(object);
}

void NCL::DistributedGameServer::ServerWorldManager::FlushPendingDeletions() {
	for (CSC8503::GameObject* object : mPendingDeletion) {
		delete object;
	}
	mPendingDeletion.clear();
}

bool NCL::DistributedGameServer::ServerWorldManager::DestroyObject(int networkObjectID,
	NCL::Interaction::DespawnReason reason, int destroyerPlayerID) {
	// Idempotent (race W4): a destroy arriving twice - once direct, once relayed -
	// observes the tombstone and reports success rather than double-destroying.
	if (IsTombstoned(networkObjectID)) {
		return true;
	}

	const auto entry = mCreatedObjectPool.find(networkObjectID);
	if (entry == mCreatedObjectPool.end() || entry->second == nullptr) {
		return false;
	}

	CSC8503::GameObject* object = entry->second;

	// Race W1: a destroy that arrives after the transition flag is set but before
	// the handoff is dispatched. The tick order runs the network pump before
	// HandleObjectTransitions, so this is the common case - and destroy wins.
	if (auto* networkObject = object->GetNetworkObject()) {
		if (networkObject->IsPendingTransition()) {
			networkObject->CancelPendingTransition();
		}
	}

	mTombstones.insert(networkObjectID);
	TeardownObject(object);
	// The pool entry becomes a tombstone rather than being erased, so a late relayed
	// command resolves to ObjectDestroyed rather than ObjectUnknown.
	entry->second = nullptr;

	PendingDespawn despawn;
	despawn.objectID = networkObjectID;
	despawn.reason = static_cast<int>(reason);
	despawn.destroyerPlayerID = destroyerPlayerID;
	mPendingDespawns.push_back(despawn);

	return true;
}

std::vector<NCL::DistributedGameServer::ServerWorldManager::ManifestEntry>
NCL::DistributedGameServer::ServerWorldManager::BuildOwnedObjectManifest() const {
	std::vector<ManifestEntry> manifest;
	manifest.reserve(mCreatedObjectPool.size());

	for (const auto& poolEntry : mCreatedObjectPool) {
		CSC8503::GameObject* object = poolEntry.second;
		// Tombstoned (null) and peer-owned (inactive) entries are both skipped: the
		// manifest describes what THIS server owns, and every other server sends its
		// own, so the joiner still ends up with the whole world exactly once.
		if (object == nullptr || !object->IsNetworkActive()) {
			continue;
		}

		// RUNTIME-spawned objects only. Pre-seeded ones need no manifest: every server
		// builds the identical set independently, and a client learns them from the
		// first snapshot. Including them meant every peer received a ~400-entry
		// reliable burst at connect time - which flooded the link during bootstrap and
		// stopped a 4-server instance starting at all.
		if (!NCL::NetworkIdSpace::IsRuntimeId(poolEntry.first)) {
			continue;
		}

		ManifestEntry entry;
		entry.objectID = poolEntry.first;
		const auto archetype = mObjectArchetypes.find(poolEntry.first);
		entry.archetypeID = (archetype != mObjectArchetypes.end()) ? archetype->second : 0;
		entry.position = object->GetTransform().GetPosition();
		manifest.push_back(entry);
	}

	return manifest;
}

bool NCL::DistributedGameServer::ServerWorldManager::PopPendingDespawn(PendingDespawn& out) {
	if (mPendingDespawns.empty()) {
		return false;
	}
	out = mPendingDespawns.front();
	mPendingDespawns.erase(mPendingDespawns.begin());
	return true;
}

void NCL::DistributedGameServer::ServerWorldManager::ApplyRemoteDespawn(int networkID, int reason,
	int destroyerPlayerID) {
	if (IsTombstoned(networkID)) {
		return;
	}
	mTombstones.insert(networkID);

	const auto entry = mCreatedObjectPool.find(networkID);
	if (entry == mCreatedObjectPool.end() || entry->second == nullptr) {
		// Race W3: the destroy beat the object here. Remember it, so when
		// StartHandlingObject later runs for this id it destroys instead of
		// activating - otherwise the object would be resurrected.
		mPendingDestroyOnArrival.insert(networkID);
		return;
	}

	if (auto* networkObject = entry->second->GetNetworkObject()) {
		networkObject->CancelPendingTransition();
	}
	TeardownObject(entry->second);
	entry->second = nullptr;
}

void NCL::DistributedGameServer::ServerWorldManager::ApplyImpulse(int networkObjectID,
	const Maths::Vector3& impulse) {
	CSC8503::GameObject* object = FindActiveObject(networkObjectID);
	if (object == nullptr || object->GetPhysicsObject() == nullptr) {
		return;
	}
	object->GetPhysicsObject()->ApplyLinearImpulse(impulse);
}

void NCL::DistributedGameServer::ServerWorldManager::ApplyRadialImpulse(const Maths::Vector3& origin,
	float radius, float magnitude) {
	if (radius <= 0.0f) {
		return;
	}
	const float radiusSquared = radius * radius;

	for (auto& entry : mCreatedObjectPool) {
		CSC8503::GameObject* object = entry.second;
		if (object == nullptr || !object->IsNetworkActive() || object->GetPhysicsObject() == nullptr) {
			continue;
		}

		const Maths::Vector3 offset = object->GetTransform().GetPosition() - origin;
		const float distanceSquared = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
		if (distanceSquared > radiusSquared || distanceSquared <= 0.0f) {
			continue;
		}

		// Linear falloff to zero at the radius, so an object exactly on the edge
		// gets nothing and the effect has no discontinuity at the boundary.
		const float distance = std::sqrt(distanceSquared);
		const float falloff = 1.0f - (distance / radius);
		const float scale = (magnitude * falloff) / distance;
		object->GetPhysicsObject()->ApplyLinearImpulse(
			Maths::Vector3(offset.x * scale, offset.y * scale, offset.z * scale));
	}
}

void NCL::DistributedGameServer::ServerWorldManager::SetMoveAxis(int networkObjectID, int playerID,
	const Maths::Vector3& axis) {
	CSC8503::GameObject* object = FindActiveObject(networkObjectID);
	if (object == nullptr || object->GetPhysicsObject() == nullptr) {
		return;
	}
	// Recorded, not applied here: the axis is continuous state. ApplyControlForces
	// re-applies it every tick, so movement depends on the input rather than on how
	// often the client happens to send it - and the state travels with the object
	// when it is handed to another server.
	object->SetControlState(playerID, axis);
}

void NCL::DistributedGameServer::ServerWorldManager::ApplyControlForces() {
	for (auto& poolEntry : mCreatedObjectPool) {
		CSC8503::GameObject* object = poolEntry.second;
		if (object == nullptr || !object->IsNetworkActive()) {
			continue;
		}
		if (object->GetControllerPlayerID() < 0 || object->GetPhysicsObject() == nullptr) {
			continue;
		}

		const Maths::Vector3& axis = object->GetMoveAxis();
		// Applied as a force so it composes with gravity and collisions rather than
		// overwriting the velocity the integrator just produced.
		object->GetPhysicsObject()->AddForce(Maths::Vector3(
			axis.x * MOVE_AXIS_FORCE, axis.y * MOVE_AXIS_FORCE, axis.z * MOVE_AXIS_FORCE));
	}
}

void NCL::DistributedGameServer::ServerWorldManager::RelayToServer(int serverID,
	NCL::Interaction::CommandType type, const NCL::Interaction::CommandArgs& args) {
	if (serverID < 0 || serverID == mServerID) {
		return;
	}
	PendingRelay relay;
	relay.targetServerID = serverID;
	relay.type = type;
	relay.args = args;
	mPendingRelays.push_back(relay);
}

bool NCL::DistributedGameServer::ServerWorldManager::PopPendingRelay(PendingRelay& out) {
	if (mPendingRelays.empty()) {
		return false;
	}
	out = mPendingRelays.front();
	mPendingRelays.erase(mPendingRelays.begin());
	return true;
}

void NCL::DistributedGameServer::ServerWorldManager::GetOverlappedServers(const Maths::Vector3& origin,
	float radius, std::vector<int>& outServerIDs) const {
	outServerIDs.clear();
	if (radius <= 0.0f || mServerBorderMap == nullptr) {
		return;
	}

	for (const auto& entry : *mServerBorderMap) {
		if (entry.first == mServerID || entry.second == nullptr) {
			continue;   // Excludes this server, per the interface contract.
		}
		const PhysicsServerBorderData* border = entry.second;

		// Closest point on the region rectangle to the sphere centre; inside the
		// radius means the sphere overlaps that region.
		const float closestX = std::clamp(origin.x, border->minXVal, border->maxXVal);
		const float closestZ = std::clamp(origin.z, border->minZVal, border->maxZVal);
		const float dx = origin.x - closestX;
		const float dz = origin.z - closestZ;

		if ((dx * dx + dz * dz) <= (radius * radius)) {
			outServerIDs.push_back(entry.first);
		}
	}
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
	// "uniform" shares shuttle's motion model; only the STARTING distribution
	// differs. Without motion an evenly-spread world produces zero handoffs, which
	// would measure partitioning with the handoff path switched off.
	if (mWorkload != "shuttle" && mWorkload != "uniform") {
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

	// Before the integrator: an object scheduled to arrive this tick must be part of
	// this tick's simulation, not the next one.
	FlushScheduledHandoffs();

	// Before the integrator, so this tick's control input contributes to this tick's
	// motion rather than arriving a frame late.
	ApplyControlForces();

	start = std::chrono::high_resolution_clock::now();
	mPhysics->Update(dt);
	end = std::chrono::high_resolution_clock::now();
	timeTaken = end - start;
	Profiler::SetPhysicsTime(timeTaken.count());

	CheckPositionOutOfServerBoundaries();

	// Freed here, at the END of the tick, because mPhysics->Update above has already
	// run FlushPendingUnregisters and purged every raw pointer to these objects from
	// mDynamicObjectList and the collision sets. Freeing any earlier would leave a
	// dangling pointer in those containers for the rest of the tick.
	FlushPendingDeletions();

	Profiler::SetHandoffsSent(mHandoffsSent);
	Profiler::SetHandoffsReceived(mHandoffsReceived);
	Profiler::SetHandoffsFailed(mHandoffsFailed);
	Profiler::SetHandoffsLate(mHandoffsLate);

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
		// Locality (I6): what this server HOLDS, as opposed to what it simulates.
		// Today both equal the world total on every server.
		sample.poolObjects = static_cast<int32_t>(mCreatedObjectPool.size());
		sample.worldObjects = static_cast<int32_t>(mGameWorld->GetGameObjects().size());
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

		// The "seam" workload centres each grid ON the world origin, where the region
		// borders meet. The grid grows in +x/+z from startPos, so centring it means
		// stepping back by half its extent - which lands a whole row and column of
		// objects EXACTLY on x=0 and z=0, with the rest straddling both sides.
		//
		// That is the case the half-open ownership rule exists for. Without it the
		// default world spawns every object well inside one region, so a pre-seed
		// ownership check passes without the border case ever arising: it confirms no
		// regression rather than confirming the rule works.
		if (mWorkload == "seam") {
			startPos.x = -(rows / 2) * OBJECT_GRID_SPACING;
			startPos.z = -(cols / 2) * OBJECT_GRID_SPACING;
		}

		float rowSpacing = OBJECT_GRID_SPACING;
		float colSpacing = OBJECT_GRID_SPACING;

		// "uniform" spreads the grid across the WHOLE world rather than clustering it
		// in one region. The shuttle workload launches every object from a single
		// start offset, so a static partition necessarily begins ~90% loaded on one
		// server - a deliberately adversarial distribution. Uniform is the balanced
		// counterpart: it measures what the partition does when the world is evenly
		// populated to begin with, which is the fair speedup case.
		if (mWorkload == "uniform") {
			float worldMinX = 0.0f, worldMaxX = 0.0f, worldMinZ = 0.0f, worldMaxZ = 0.0f;
			if (GetWorldExtent(worldMinX, worldMaxX, worldMinZ, worldMaxZ)) {
				// A margin keeps objects off the outer edge, where the closed-boundary
				// rule and the floor edge would both come into play.
				constexpr float UNIFORM_EDGE_MARGIN = 0.1f;
				const float usableX = (worldMaxX - worldMinX) * (1.0f - 2.0f * UNIFORM_EDGE_MARGIN);
				const float usableZ = (worldMaxZ - worldMinZ) * (1.0f - 2.0f * UNIFORM_EDGE_MARGIN);

				rowSpacing = (rows > 1) ? (usableX / static_cast<float>(rows - 1)) : 0.0f;
				colSpacing = (cols > 1) ? (usableZ / static_cast<float>(cols - 1)) : 0.0f;

				startPos.x = worldMinX + (worldMaxX - worldMinX) * UNIFORM_EDGE_MARGIN;
				startPos.z = worldMinZ + (worldMaxZ - worldMinZ) * UNIFORM_EDGE_MARGIN;
				startPos.y = 10.f;
			}
		}

		CreateObjectGrid(rows, cols, objectsPerPlayer, rowSpacing, colSpacing, i, startPos);
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
	if (packet == nullptr) {
		return false;
	}

	// Apply on arrival unless a lookahead is configured.
	if (mHandoffLookaheadTicks <= 0) {
		return ApplyIncomingObject(packet);
	}

	// The sender released the object on a deterministic tick, so scheduling the
	// application relative to THAT rather than to arrival time makes the handoff
	// land on the same tick in every run - no barrier, no inter-server coordination.
	const uint64_t applyAt =
		static_cast<uint64_t>(packet->mSenderTick) + static_cast<uint64_t>(mHandoffLookaheadTicks);

	if (applyAt <= mTickCounter) {
		// Arrived too late to make its slot. Applied immediately so the object is not
		// lost, but counted: a non-zero total means this run is NOT reproducible.
		++mHandoffsLate;
		return ApplyIncomingObject(packet);
	}

	ScheduledHandoff scheduled;
	scheduled.packet = std::make_unique<StartSimulatingObjectPacket>(*packet);
	scheduled.applyAtTick = applyAt;
	mScheduledHandoffs.push_back(std::move(scheduled));
	return true;
}

void DistributedGameServer::ServerWorldManager::FlushScheduledHandoffs() {
	// Deterministic tie-break for simultaneous events. mScheduledHandoffs is in packet
	// ARRIVAL order, so two transfers scheduled for the same tick were applied in
	// whichever order ENet happened to deliver them. Application order decides the
	// order objects are reactivated and therefore their order in the broadphase pair
	// list, and contact resolution is order dependent - so arrival order leaked into
	// the simulated result. Object IDs are globally unique by construction
	// (NetworkIdSpace.h), so (applyAtTick, objectID) is a total order and is free.
	std::sort(mScheduledHandoffs.begin(), mScheduledHandoffs.end(),
		[](const ScheduledHandoff& l, const ScheduledHandoff& r) {
			if (l.applyAtTick != r.applyAtTick) {
				return l.applyAtTick < r.applyAtTick;
			}
			return l.packet->objectID < r.packet->objectID;
		});

	for (auto entry = mScheduledHandoffs.begin(); entry != mScheduledHandoffs.end(); ) {
		if (entry->applyAtTick > mTickCounter) {
			// Sorted by tick, so nothing later in the vector is due either.
			break;
		}
		ApplyIncomingObject(entry->packet.get());
		entry = mScheduledHandoffs.erase(entry);
	}
}

bool DistributedGameServer::ServerWorldManager::ApplyIncomingObject(StartSimulatingObjectPacket* packet) {
	// Checked BEFORE any construction. The object was destroyed while in flight
	// (races W3/W4); building it here and tearing it down again would resurrect it
	// for the length of this function, and on the construct-on-arrival path it would
	// also register it with the physics system mid-tick. Counted as received - the
	// sender genuinely did release it, so dropping it silently would break handoff
	// parity (I5) instead.
	if (IsTombstoned(packet->objectID) ||
		mPendingDestroyOnArrival.find(packet->objectID) != mPendingDestroyOnArrival.end()) {
		mPendingDestroyOnArrival.erase(packet->objectID);
		mTombstones.insert(packet->objectID);
		auto existing = mCreatedObjectPool.find(packet->objectID);
		if (existing != mCreatedObjectPool.end() && existing->second != nullptr) {
			TeardownObject(existing->second);
			existing->second = nullptr;
		}
		++mHandoffsReceived;
		std::cout << "Handoff for destroyed object " << packet->objectID
			<< " - dropped rather than resurrected.\n";
		return true;
	}

	auto poolEntry = mCreatedObjectPool.find(packet->objectID);
	if (poolEntry == mCreatedObjectPool.end()) {
		// Not known here. Under the pre-seed model this was an error, because every
		// server held a deactivated twin of every object and an unknown id meant the
		// world sets had diverged. Once a server holds only its own region, an
		// incoming object it has never seen is the NORMAL case, so it is built from
		// the archetype the packet carries.
		GameObject* built = CreateObjectFromArchetype(packet->mArchetypeID,
			packet->lastFullState.predictedPosition, packet->objectID, packet->mControllerPlayerID);
		if (built == nullptr) {
			++mHandoffsFailed;
			std::cout << "ERROR: could not build incoming object " << packet->objectID
				<< " from archetype " << packet->mArchetypeID << "\n";
			return false;
		}
		// Built deactivated; the shared path below activates it after applying state,
		// so a constructed object and a reactivated twin follow identical code.
		built->SetActive(false);
		poolEntry = mCreatedObjectPool.find(packet->objectID);
	}
	// A tombstoned-then-nulled entry: the id is known but the object is gone. Treated
	// as a failure rather than rebuilt, because reaching here means the tombstone
	// check above did not fire, which would be a real inconsistency.
	if (poolEntry == mCreatedObjectPool.end() || poolEntry->second == nullptr) {
		++mHandoffsFailed;
		std::cout << "ERROR: handoff for object " << packet->objectID
			<< " resolved to a null pool entry.\n";
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

		// Continuous input travels with the object, so a driven avatar keeps moving
		// across the border instead of stalling until the client's next axis update.
		objectToHandle->SetControlState(packet->mControllerPlayerID, packet->mMoveAxis);

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

			const bool isCube = (DeterministicHash(mWorldSeed, playerID, objCounter) & 1u) != 0;
			if (isCube) {
				std::cout << "Creating Object at: " << transform.GetPosition() << "\n";
				obj = AddCubeToWorld(transform, objCounter++, playerID);
			}
			else {
				obj = AddSphereToWorld(transform, objCounter++, playerID);
			}

			AddNetworkObject(*obj);
			auto networkId = obj->GetNetworkObject()->GetNetworkID();
			mCreatedObjectPool[networkId] = obj;
			// Recorded for pre-seeded objects too: without it a late joiner would be
			// told every existing object is the default archetype.
			mObjectArchetypes[networkId] = static_cast<int>(isCube
				? NCL::Interaction::ObjectArchetype::Cube
				: NCL::Interaction::ObjectArchetype::Sphere);

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

const std::vector<NCL::Interaction::RegionBounds>&
DistributedGameServer::ServerWorldManager::GetRegionBounds() const {
	const size_t mapSize = (mServerBorderMap != nullptr) ? mServerBorderMap->size() : 0;
	if (mCachedRegions.size() != mapSize) {
		mCachedRegions.clear();
		mCachedRegions.reserve(mapSize);
		if (mServerBorderMap != nullptr) {
			for (const auto& entry : *mServerBorderMap) {
				if (entry.second == nullptr) {
					continue;
				}
				mCachedRegions.push_back(NCL::Interaction::RegionBounds{
					entry.first,
					entry.second->minXVal, entry.second->maxXVal,
					entry.second->minZVal, entry.second->maxZVal });
			}
		}
	}
	return mCachedRegions;
}

// The union of every server's region: the world's outer bounds. Derived from the
// same border map ownership uses, so a workload can never place an object outside
// the partition it is being measured against.
bool DistributedGameServer::ServerWorldManager::GetWorldExtent(float& minX, float& maxX,
	float& minZ, float& maxZ) const {
	const auto& regions = GetRegionBounds();
	if (regions.empty()) {
		return false;
	}

	minX = regions[0].minX; maxX = regions[0].maxX;
	minZ = regions[0].minZ; maxZ = regions[0].maxZ;
	for (const auto& region : regions) {
		if (region.minX < minX) minX = region.minX;
		if (region.maxX > maxX) maxX = region.maxX;
		if (region.minZ < minZ) minZ = region.minZ;
		if (region.maxZ > maxZ) maxZ = region.maxZ;
	}
	return true;
}

// Delegates rather than testing mServerBorderData directly, so it cannot disagree
// with GetObjectServer. It used to: this test was half-open on X but CLOSED on Z,
// while GetObjectServer was closed on both, so a point on a shared border was
// claimed by the handoff path and rejected by the pre-seed path.
bool DistributedGameServer::ServerWorldManager::IsObjectInBorder(const Maths::Vector3& objectPosition) const {
	return GetObjectServer(objectPosition) == mServerID;
}

// The single ownership authority for this process.
int DistributedGameServer::ServerWorldManager::GetObjectServer(const Maths::Vector3& position) const {
	return NCL::Interaction::OwningServerFor(GetRegionBounds(), position);
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
