#include "ServerWorldManager.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "GameWorld.h"
#include "NetworkObject.h"
#include "PhysicsObject.h"
#include "PhysicsSystem.h"
#include "DistributedSystemCommonFiles/TaskPool.h"
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

	// "headon" workload. Pairs start this far either side of x = 0 and are launched
	// straight at each other, so every pair meets exactly on the region border. The
	// gap is large enough that the pair is well inside its own region at t=0 and the
	// collision is unambiguously a border event rather than a spawn overlap.
	//
	// Close together, and only just fast enough. Two constraints pin these:
	//
	//  - too slow and the pair lands before it reaches the border and ground friction
	//    bleeds the velocity off. At 20 u/s over a 40-unit gap no collision happened
	//    even on a single server.
	//  - too fast and the pair tunnels. The engine has no continuous collision
	//    detection, so a pair closing faster than the sum of their half-extents per
	//    substep can step straight past each other. At 60 u/s each, closing 1.0 units
	//    per 120 Hz substep against a 1.0-unit contact window, even the SINGLE-server
	//    case is marginal - which measures the integrator's discrete-collision limit
	//    rather than anything about region borders.
	//
	// 30 u/s each closes 0.5 units per substep, so a pair overlaps for two substeps
	// and the contact is unambiguous. The gap is shortened to match, so they still
	// meet while airborne.
	constexpr float HEADON_HALF_GAP = 12.0f;
	constexpr float HEADON_SPEED = 30.0f;
	// Wide enough that neighbouring pairs never reach each other, so every contact
	// recorded is the head-on one and the count is exactly the number of pairs that
	// actually collided.
	constexpr float HEADON_LANE_SPACING = 6.0f;

	// "cluster" workload: objects packed into one part of the world, milling about but
	// not migrating.
	//
	// This is the workload a load balancer should actually be judged on, and none of
	// the others are. "uniform" is already balanced, so there is nothing to correct.
	// "shuttle" is unbalanced but every object is sweeping across the world, so the
	// load distribution moves as fast as a border can - a policy measuring the last
	// interval is always correcting towards where the load WAS. A cluster is unbalanced
	// and STATIONARY, which separates "can the policy find the right partition" from
	// "can it track a moving one".
	//
	// It is also what a real game world looks like: players gather in places and stay
	// there, rather than sweeping the map in formation.
	constexpr float CLUSTER_EXTENT_FRACTION = 0.22f;
	constexpr float CLUSTER_CENTRE_FRACTION = 0.28f;
	// Small enough that an object stays inside the cluster for the length of a run,
	// large enough that the objects interact rather than settling into a static heap -
	// contacts are the load measure, so a cluster with no contacts would not be a load.
	constexpr float CLUSTER_SPEED = 4.0f;

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

void NCL::DistributedGameServer::ServerWorldManager::SetPhysicsWorkerThreads(int workerCount) {
	// Negative asks for the hardware default; 0 stays serial. Set before the world is
	// built, so the pool exists for the very first tick.
	const int workers = (workerCount < 0) ? NCL::TaskPool::DefaultWorkerCount() : workerCount;
	mPhysics->SetWorkerThreadCount(workers);
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

bool NCL::DistributedGameServer::ServerWorldManager::TryGetLastKnownOwner(int networkObjectID,
	int& outServerID) const {
	const auto entry = mLastKnownOwner.find(networkObjectID);
	if (entry == mLastKnownOwner.end()) {
		return false;
	}
	outServerID = entry->second;
	return true;
}

void NCL::DistributedGameServer::ServerWorldManager::RecordObjectOwner(int networkObjectID,
	int serverID) {
	if (networkObjectID < 0 || serverID < 0) {
		return;
	}
	// Last write wins. An entry naming THIS server is still worth keeping: it is what
	// tells a later handoff-out that the object was ours, and ResolveForwardTarget
	// ignores self-entries rather than forwarding in a loop.
	mLastKnownOwner[networkObjectID] = serverID;
	mLastKnownOwnerTick[networkObjectID] = mTickCounter;
}

// Drops forwarding entries older than a client's plausible staleness window.
//
// An entry exists so a command aimed at an object this server has handed away can
// still be forwarded rather than rejected. That is only useful while the CLIENT's view
// can still be that out of date, which is a few seconds - a client learns the new
// owner from the next snapshot or from a NotOwner ack. Beyond that the entry is dead
// weight, and it used to accumulate for the life of the process: one per object ever
// handed away, never removed.
void DistributedGameServer::ServerWorldManager::PruneForwardingTable() {
	// Ten seconds at the substep rate. Far longer than any client's view can lag, and
	// short enough that a long run does not accumulate. Erring long on purpose: an
	// entry pruned too early turns a forwardable command into a rejected one, which is
	// a correctness regression, while one pruned too late costs eight bytes.
	constexpr uint64_t FORWARD_ENTRY_LIFETIME_TICKS = 1200;
	if (mTickCounter < FORWARD_ENTRY_LIFETIME_TICKS) {
		return;
	}
	const uint64_t cutoff = mTickCounter - FORWARD_ENTRY_LIFETIME_TICKS;

	for (auto entry = mLastKnownOwnerTick.begin(); entry != mLastKnownOwnerTick.end(); ) {
		if (entry->second > cutoff) {
			++entry;
			continue;
		}
		// Never prune an entry for an object this server currently owns: that one is
		// not a forwarding hint, it is the record that says the object was ours, and
		// HandleOutgoingObject reads it when the object eventually leaves.
		const auto owned = mCreatedObjectPool.find(entry->first);
		if (owned != mCreatedObjectPool.end() && owned->second != nullptr) {
			entry->second = mTickCounter;   // Refresh rather than drop.
			++entry;
			continue;
		}
		mLastKnownOwner.erase(entry->first);
		entry = mLastKnownOwnerTick.erase(entry);
	}
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

	// The owner activates. Peers build nothing at all - they take only the owner id
	// off the spawn broadcast - so a later handoff constructs the object on arrival
	// rather than reactivating a copy that was sitting there all along.
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

bool NCL::DistributedGameServer::ServerWorldManager::RecordRemoteSpawn(int networkID,
	int ownerServerID) {
	// The spawn broadcast can lose a race with a handoff: the owner spawns an object,
	// broadcasts, and hands it to us before the broadcast lands. Recording the packet's
	// owner then would point commands for an object we are actively simulating back at
	// the server that no longer has it.
	const auto entry = mCreatedObjectPool.find(networkID);
	if (entry != mCreatedObjectPool.end() && entry->second != nullptr) {
		return false;
	}

	// Nothing is constructed. A non-owner now holds one map entry rather than a
	// GameObject, a PhysicsObject, a NetworkObject and a slot in every physics list -
	// which is the whole point of the region-local model. Handoff builds the object if
	// it ever arrives (ApplyIncomingObject); until then this is all a peer needs to
	// forward a command to whoever does own it.
	RecordObjectOwner(networkID, ownerServerID);
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

// --- halo band publication ---------------------------------------------------

// The fastest any bundled workload launches an object (headon). A constant rather
// than the measured maximum on purpose: a band width derived from live velocities
// would change with the contents of the world, and two servers computing different
// widths would publish different sets.
static constexpr float HALO_ASSUMED_MAX_SPEED = 60.0f;
// Generous compared with the unit cubes the workloads build, so a pair is published
// well before it can touch.
static constexpr float HALO_ASSUMED_MAX_RADIUS = 2.0f;

float DistributedGameServer::ServerWorldManager::MinimumSafeHaloWidth() const {
	const int substepHz = (mPhysics != nullptr) ? mPhysics->GetSubstepHZ() : 120;
	const float substepDt = (substepHz > 0) ? (1.0f / static_cast<float>(substepHz)) : (1.0f / 120.0f);

	// Distance an object can cover between its state being sampled and that state
	// being applied on the neighbour, plus room for both bodies.
	const float lag = static_cast<float>(std::max(0, mHaloLookaheadTicks)) * substepDt;
	return HALO_ASSUMED_MAX_SPEED * lag + 2.0f * HALO_ASSUMED_MAX_RADIUS;
}

void DistributedGameServer::ServerWorldManager::SetHaloWidth(float width) {
	mHaloWidth = width;
	if (width <= 0.0f) {
		return;   // Disabled; every measurement before this increment ran this way.
	}

	const float floorWidth = MinimumSafeHaloWidth();
	if (width < floorWidth) {
		// Loud, because the symptom is not a crash. It is a contact that is missed
		// only when an object happens to cross the band faster than the update rate,
		// which varies with load and would read as flakiness rather than as a
		// misconfiguration.
		std::cout << "WARNING: --halo-width " << width << " is below the safe minimum "
			<< floorWidth << " for a halo lookahead of " << mHaloLookaheadTicks
			<< " ticks. Border contacts may be missed.\n";
	}
}

bool DistributedGameServer::ServerWorldManager::TryGetHaloState(int objectID,
	CSC8503::HaloObjectState& state) const {
	const auto entry = mCreatedObjectPool.find(objectID);
	if (entry == mCreatedObjectPool.end() || entry->second == nullptr) {
		return false;
	}
	CSC8503::GameObject* object = entry->second;
	// A shadow is someone else's object; republishing it would echo it back to its
	// owner and, on a three-server corner, around the mesh.
	if (object->IsHaloShadow() || !object->IsNetworkActive()) {
		return false;
	}
	auto* physics = object->GetPhysicsObject();
	if (physics == nullptr) {
		return false;
	}

	state.objectID = objectID;
	state.archetypeID = GetObjectArchetype(objectID);
	state.position = object->GetTransform().GetPosition();
	state.orientation = object->GetTransform().GetOrientation();
	state.linearVelocity = physics->GetLinearVelocity();
	state.angularVelocity = physics->GetAngularVelocity();
	return true;
}

void DistributedGameServer::ServerWorldManager::CollectHaloPublications(
	std::vector<HaloPublication>& out) const {
	out.clear();
	if (mHaloWidth <= 0.0f) {
		return;
	}

	std::vector<int> overlapped;
	for (const auto& entry : mCreatedObjectPool) {
		CSC8503::GameObject* object = entry.second;
		if (object == nullptr || object->IsHaloShadow() || !object->IsNetworkActive()) {
			continue;
		}

		// Which OTHER regions this object is within mHaloWidth of. Exactly the query
		// an area effect uses to find the regions a blast reaches, with the radius
		// being the band width - reused rather than rewritten, because a second
		// border test that disagrees with the first is the failure mode the
		// ownership unification existed to remove.
		GetOverlappedServers(object->GetTransform().GetPosition(), mHaloWidth, overlapped);

		for (int targetServerID : overlapped) {
			HaloPublication publication;
			publication.targetServerID = targetServerID;
			publication.objectID = entry.first;
			out.push_back(publication);
		}
	}

	// Deterministic order. mCreatedObjectPool is a std::map so it is already ordered
	// by object id, but the per-object neighbour list is appended in region-map order;
	// sorting by (target, object) makes the batching below independent of both.
	std::sort(out.begin(), out.end(), [](const HaloPublication& l, const HaloPublication& r) {
		if (l.targetServerID != r.targetServerID) {
			return l.targetServerID < r.targetServerID;
		}
		return l.objectID < r.objectID;
	});
}

void DistributedGameServer::ServerWorldManager::ScheduleHaloUpdate(
	const CSC8503::HaloObjectState& state, int senderTick, int senderServerID) {
	// We own it. Happens legitimately around a handoff: the previous owner samples an
	// object, hands it to us, and its update lands afterwards. Shadowing an object we
	// simulate would put two copies of it in the broadphase.
	const auto owned = mCreatedObjectPool.find(state.objectID);
	if (owned != mCreatedObjectPool.end() && owned->second != nullptr) {
		return;
	}
	if (IsTombstoned(state.objectID)) {
		return;
	}

	ScheduledHaloUpdate scheduled;
	scheduled.objectID = state.objectID;
	scheduled.archetypeID = state.archetypeID;
	scheduled.ownerServerID = senderServerID;
	scheduled.state.position = state.position;
	scheduled.state.linearVelocity = state.linearVelocity;
	scheduled.state.angularVelocity = state.angularVelocity;
	scheduled.state.orientation = state.orientation;
	scheduled.state.ownerServerID = senderServerID;
	scheduled.state.sampleTick = static_cast<uint64_t>(std::max(0, senderTick));

	const uint64_t applyAt =
		static_cast<uint64_t>(senderTick) + static_cast<uint64_t>(std::max(0, mHaloLookaheadTicks));

	if (applyAt <= mTickCounter) {
		// Missed its slot. Applied anyway - a shadow frozen at an old position is
		// worse than one that jumps - but counted, because a non-zero total means the
		// halo lookahead is too small for the actual delivery jitter and the run is
		// not reproducible.
		++mHaloUpdatesLate;
		scheduled.applyAtTick = mTickCounter;
	}
	else {
		scheduled.applyAtTick = applyAt;
	}
	mScheduledHaloUpdates.push_back(std::move(scheduled));
}

void DistributedGameServer::ServerWorldManager::FlushScheduledHaloUpdates() {
	if (mScheduledHaloUpdates.empty()) {
		return;
	}

	// Same total order as FlushScheduledHandoffs, and for the same reason: the vector
	// is in packet ARRIVAL order, and the order shadows are created in decides their
	// order in the broadphase pair list, which contact resolution is sensitive to.
	std::sort(mScheduledHaloUpdates.begin(), mScheduledHaloUpdates.end(),
		[](const ScheduledHaloUpdate& l, const ScheduledHaloUpdate& r) {
			if (l.applyAtTick != r.applyAtTick) {
				return l.applyAtTick < r.applyAtTick;
			}
			return l.objectID < r.objectID;
		});

	for (auto entry = mScheduledHaloUpdates.begin(); entry != mScheduledHaloUpdates.end(); ) {
		if (entry->applyAtTick > mTickCounter) {
			break;   // Sorted, so nothing after this is due either.
		}

		// Re-checked here, not only at schedule time: the object may have been handed
		// to us, or destroyed, during the lookahead window.
		const auto owned = mCreatedObjectPool.find(entry->objectID);
		const bool nowOurs = (owned != mCreatedObjectPool.end() && owned->second != nullptr);
		if (nowOurs || IsTombstoned(entry->objectID)) {
			entry = mScheduledHaloUpdates.erase(entry);
			continue;
		}

		entry->state.lastAppliedTick = mTickCounter;
		mHaloState[entry->objectID] = entry->state;

		if (mHaloObjects.find(entry->objectID) == mHaloObjects.end()) {
			CreateHaloShadow(entry->archetypeID, entry->objectID, entry->state);
		}

		entry = mScheduledHaloUpdates.erase(entry);
	}
}

CSC8503::GameObject* DistributedGameServer::ServerWorldManager::CreateHaloShadow(
	int archetypeID, int networkID, const HaloAuthoritativeState& state) {
	Transform transform;
	transform.SetPosition(state.position);
	transform.SetOrientation(state.orientation);

	GameObject* object = (archetypeID == static_cast<int>(NCL::Interaction::ObjectArchetype::Sphere))
		? AddSphereToWorld(transform, networkID, -1)
		: AddCubeToWorld(transform, networkID, -1);
	if (object == nullptr) {
		return nullptr;
	}

	// No NetworkObject. That is not an omission: the snapshot loop and the border
	// check both iterate mNetworkObjects, so leaving a shadow out of it is what stops
	// this server broadcasting someone else's object to clients or trying to hand it
	// away. Command targeting is covered too, since FindActiveObject reads the pool.
	object->SetIsHaloShadow(true);

	// Set explicitly, because a shadow deliberately has no NetworkObject to carry it.
	// Without this the pair would be oriented by world id, which is a local creation
	// counter - the owning server builds this object at pre-seed and its neighbour
	// builds the shadow later, so the two would orient the same contact oppositely and
	// compute different impulses from it (invariant I8).
	object->SetContactOrderID(networkID);

	// Recorded now, because if this shadow is later promoted the object becomes ours
	// and a subsequent handoff has to be able to name its shape.
	mObjectArchetypes[networkID] = archetypeID;

	if (auto* physics = object->GetPhysicsObject()) {
		physics->SetLinearVelocity(state.linearVelocity);
		physics->SetAngularVelocity(state.angularVelocity);
	}

	mGameWorld->AddGameObject(object);
	mPhysics->RegisterObject(object);
	mHaloObjects[networkID] = object;
	return object;
}

void DistributedGameServer::ServerWorldManager::ReimposeHaloState() {
	const int substepHz = (mPhysics != nullptr) ? mPhysics->GetSubstepHZ() : 120;
	const float substepDt = (substepHz > 0) ? (1.0f / static_cast<float>(substepHz)) : (1.0f / 120.0f);

	for (const auto& entry : mHaloObjects) {
		GameObject* object = entry.second;
		if (object == nullptr) {
			continue;
		}
		const auto found = mHaloState.find(entry.first);
		if (found == mHaloState.end()) {
			continue;
		}
		const HaloAuthoritativeState& state = found->second;

		// Dead reckoning from the sample tick to this one, rather than applying the
		// sample as-is. Two reasons it has to be here rather than a smaller lookahead:
		//
		//  - the lag is not the lookahead, it is the lookahead PLUS however long the
		//    packet took. Shrinking the lookahead to hide the lag just makes updates
		//    arrive after their slot: at a one-tick lookahead essentially every update
		//    was late, and the shadows became so inconsistent between the two servers
		//    that a headon run pushed all 100 objects onto one of them.
		//  - extrapolation is a pure function of the received state and two tick
		//    numbers, so it costs nothing in determinism, which applying-on-arrival
		//    would have.
		//
		// Gravity is deliberately not integrated here. Over the few ticks this spans
		// the 0.5*g*t^2 term is under a hundredth of a unit, and including it would
		// tie the shadow's path to a gravity setting the owner might not share.
		// Bounded. Under a badly balanced partition an overloaded server falls behind
		// its peer in real time, so its samples arrive with a tick number far below
		// the receiver's counter - on the shuttle workload the gap reaches thousands
		// of ticks. Extrapolating that far would fling the shadow across the world on
		// a velocity that is long out of date. Clamping keeps the error bounded and
		// leaves haloLate to report that the configuration is wrong, rather than
		// turning a pacing problem into a physics one.
		const uint64_t rawElapsed = (mTickCounter > state.sampleTick)
			? (mTickCounter - state.sampleTick) : 0;
		const uint64_t maxElapsed = static_cast<uint64_t>(std::max(1, mHaloLookaheadTicks)) * 3u;
		const uint64_t elapsedTicks = (rawElapsed < maxElapsed) ? rawElapsed : maxElapsed;
		const float elapsed = static_cast<float>(elapsedTicks) * substepDt;

		const Maths::Vector3 predicted = state.position + state.linearVelocity * elapsed;

		// Overwrites whatever the previous tick's contact resolution did. Both halves
		// matter: SeperateObjects moved the transform to resolve penetration, and
		// ImpulseResolveCollision wrote velocity - a shadow left carrying either would
		// diverge from the object its owner is actually simulating, and the two
		// servers would then compute different impulses from it (invariant I8).
		object->GetTransform().SetPosition(predicted);
		object->GetTransform().SetOrientation(state.orientation);
		if (auto* physics = object->GetPhysicsObject()) {
			physics->SetLinearVelocity(state.linearVelocity);
			physics->SetAngularVelocity(state.angularVelocity);
			// A force accumulated from a contact would be integrated by nobody, but
			// clearing it keeps the shadow's state exactly what its owner sent.
			physics->ClearForces();
		}
	}
}

// A shadow is only meaningful while its owner keeps publishing it. Generous, because
// halo updates are unreliable by design and a run of dropped packets must not retire
// a shadow that is still very much there; the cost of being late to retire is a few
// ticks of a ghost, and the cost of being early is a missed contact.
static constexpr uint64_t HALO_STALE_TICKS = 30;

void DistributedGameServer::ServerWorldManager::RetireStaleHaloShadows() {
	for (auto entry = mHaloObjects.begin(); entry != mHaloObjects.end(); ) {
		const auto state = mHaloState.find(entry->first);
		const uint64_t lastTick = (state != mHaloState.end()) ? state->second.lastAppliedTick : 0;

		if (mTickCounter > lastTick && (mTickCounter - lastTick) > HALO_STALE_TICKS) {
			TeardownObject(entry->second);
			mHaloState.erase(entry->first);
			entry = mHaloObjects.erase(entry);
			continue;
		}
		++entry;
	}
}

void DistributedGameServer::ServerWorldManager::RemoveHaloShadow(int networkID) {
	const auto entry = mHaloObjects.find(networkID);
	if (entry == mHaloObjects.end()) {
		return;
	}
	// The object has become ours. Without this it would be in the broadphase twice -
	// once as the object we simulate and once as a shadow sitting where it used to be.
	TeardownObject(entry->second);
	mHaloObjects.erase(entry);
	mHaloState.erase(networkID);
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
	auto* physicsComp = obj.GetPhysicsObject();
	if (physicsComp == nullptr) {
		return;
	}

	if (mWorkload == "cluster") {
		// Milling, not migrating: direction varies per object but the speed is low
		// enough that the cluster keeps its shape for the length of a run.
		const unsigned int h = DeterministicHash(mWorldSeed ^ 0x5EED1234u, playerID, objectIndex);
		const float angle = (static_cast<float>(h % 3600u) / 3600.0f) * 6.2831853f;
		physicsComp->SetLinearVelocity(Maths::Vector3(
			std::cos(angle) * CLUSTER_SPEED, 0.0f, std::sin(angle) * CLUSTER_SPEED));
		return;
	}

	if (mWorkload == "headon") {
		// Direction from the object's own position rather than its grid index: the
		// index-to-row mapping depends on how SetupWorld shaped the grid, and reading
		// it back here would be a second place to keep that in step. Sign of x is the
		// same answer and cannot drift.
		const float x = obj.GetTransform().GetPosition().x;
		physicsComp->SetLinearVelocity(
			Maths::Vector3((x < 0.0f) ? HEADON_SPEED : -HEADON_SPEED, 0.0f, 0.0f));
		return;
	}

	// "uniform" shares shuttle's motion model; only the STARTING distribution
	// differs. Without motion an evenly-spread world produces zero handoffs, which
	// would measure partitioning with the handoff path switched off.
	if (mWorkload != "shuttle" && mWorkload != "uniform") {
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

	// Same reason, and in this order: a halo update due this tick creates or refreshes
	// the shadow's authoritative state, then ReimposeHaloState writes that state over
	// whatever last tick's contact resolution left behind. Doing it the other way
	// round would re-impose the state the new update was about to replace.
	// Before everything else that reads a border. The partition must change for the
	// whole of the tick it takes effect on, or the ownership answers within that tick
	// would come from two different partitions.
	// Cheap and bounded: the table is small, and this is what stops it growing for
	// the life of the process.
	PruneForwardingTable();

	FlushPendingPartitions();

	// Same tick the receiving server installs the object on, so ownership changes
	// atomically rather than leaving a gap the width of the lookahead.
	FlushScheduledReleases();

	// After the releases, so a transfer scheduled and released this tick is recorded
	// before its retry window is ever evaluated.
	FlushPendingTransfers();

	FlushScheduledHaloUpdates();
	RetireStaleHaloShadows();
	ReimposeHaloState();

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

	Profiler::SetHandoffsLate(mHandoffsLate);
	Profiler::SetHaloUpdatesLate(mHaloUpdatesLate);
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
		sample.forwardEntries = static_cast<int32_t>(mLastKnownOwner.size());
		sample.contacts = static_cast<int32_t>(Profiler::GetContactsResolved());
		mContactsSinceReport += Profiler::GetContactsResolved();
		sample.haloObjects = static_cast<int32_t>(mHaloObjects.size());
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
		int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(objectsPerPlayer)))));
		int rows = std::max(1, static_cast<int>(std::ceil(static_cast<double>(objectsPerPlayer) / cols)));

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

		// "headon" is not a square grid at all: exactly two rows, at x = -gap and
		// x = +gap, spread along z. Every object therefore has a partner directly
		// opposite it across the border and nothing else within reach.
		//
		// This exists because the cross-border collision gap is otherwise measured as
		// a small difference between two large contact totals. Here it is the whole
		// signal: on one server every pair collides, on two servers - one region each
		// side of x = 0 - no pair collides at all, because neither server holds both
		// halves of any pair.
		// Packed into one part of the world, off-centre so the default partition splits
		// it badly - which is the situation a balancer exists for.
		if (mWorkload == "cluster") {
			float worldMinX = 0.0f, worldMaxX = 0.0f, worldMinZ = 0.0f, worldMaxZ = 0.0f;
			if (GetWorldExtent(worldMinX, worldMaxX, worldMinZ, worldMaxZ)) {
				const float spanX = (worldMaxX - worldMinX) * CLUSTER_EXTENT_FRACTION;
				const float spanZ = (worldMaxZ - worldMinZ) * CLUSTER_EXTENT_FRACTION;
				rowSpacing = (rows > 1) ? (spanX / static_cast<float>(rows - 1)) : 0.0f;
				colSpacing = (cols > 1) ? (spanZ / static_cast<float>(cols - 1)) : 0.0f;
				startPos.x = worldMinX + (worldMaxX - worldMinX) * CLUSTER_CENTRE_FRACTION
					- spanX * 0.5f;
				startPos.z = worldMinZ + (worldMaxZ - worldMinZ) * 0.5f - spanZ * 0.5f;
				startPos.y = 10.f;
			}
		}

		if (mWorkload == "headon") {
			rows = 2;
			cols = std::max(1, (objectsPerPlayer + 1) / 2);
			rowSpacing = 2.0f * HEADON_HALF_GAP;
			colSpacing = HEADON_LANE_SPACING;
			startPos.x = -HEADON_HALF_GAP;
			startPos.z = -(cols / 2) * HEADON_LANE_SPACING;
			startPos.y = 10.f;
		}

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


void DistributedGameServer::ServerWorldManager::SchedulePartitionChange(
	const PendingPartition& partition) {
	if (partition.regions.empty()) {
		return;
	}
	mPendingPartitions.push_back(partition);
}

void DistributedGameServer::ServerWorldManager::FlushPendingPartitions() {
	if (mPendingPartitions.empty()) {
		return;
	}

	// Oldest effective tick first, so two partitions queued together are adopted in
	// the order they were meant to take effect rather than the order they arrived.
	std::sort(mPendingPartitions.begin(), mPendingPartitions.end(),
		[](const PendingPartition& l, const PendingPartition& r) {
			return l.effectiveTick < r.effectiveTick;
		});

	for (auto entry = mPendingPartitions.begin(); entry != mPendingPartitions.end(); ) {
		if (static_cast<uint64_t>(entry->effectiveTick) > mTickCounter) {
			break;   // Sorted, so nothing later is due either.
		}

		if (static_cast<uint64_t>(entry->effectiveTick) < mTickCounter) {
			// Adopted anyway. Refusing a late partition would leave this server on one
			// nobody else is using, and every ownership question would then be
			// answered differently here than everywhere else - unrecoverable, where a
			// late switch is merely wrong for the ticks it was late by.
			++mRepartitionsLate;
			std::cout << "WARNING: partition for tick " << entry->effectiveTick
				<< " arrived at tick " << mTickCounter << "; adopting late.\n";
		}

		for (const NCL::Interaction::RegionBounds& region : entry->regions) {
			// Contents overwritten, pointers left alone. ServerWorldManager holds a
			// PhysicsServerBorderData& taken at construction and TestObject copies one,
			// so replacing the structs would dangle or silently stale those.
			auto existing = mServerBorderMap->find(region.serverId);
			if (existing == mServerBorderMap->end() || existing->second == nullptr) {
				auto* created = new PhysicsServerBorderData();
				created->minXVal = region.minX;
				created->maxXVal = region.maxX;
				created->minZVal = region.minZ;
				created->maxZVal = region.maxZ;
				(*mServerBorderMap)[region.serverId] = created;
				continue;
			}

			existing->second->minXVal = region.minX;
			existing->second->maxXVal = region.maxX;
			existing->second->minZVal = region.minZ;
			existing->second->maxZVal = region.maxZ;
		}

		// Ownership answers come from a cached copy of the border map, and the cache
		// only noticed a change in the map's SIZE. Without this the borders move for
		// the halo, which reads the map directly, and not for GetObjectServer.
		mRegionsDirty = true;

		++mRepartitionCount;
		std::cout << "Partition adopted at tick " << mTickCounter << ": ";
		for (const NCL::Interaction::RegionBounds& region : entry->regions) {
			std::cout << "[" << region.serverId << " x " << region.minX << ".." << region.maxX
				<< " z " << region.minZ << ".." << region.maxZ << "] ";
		}
		std::cout << "\n";

		entry = mPendingPartitions.erase(entry);
	}
}

bool DistributedGameServer::ServerWorldManager::TakeLoadReport(long long& outTick,
	int& outOwned, long long& outContacts, float& outMinX, float& outMaxX, int* outBuckets) {
	if (mLoadReportIntervalTicks <= 0) {
		return false;
	}
	if (mTickCounter < mLastLoadReportTick + static_cast<uint64_t>(mLoadReportIntervalTicks)) {
		return false;
	}

	outTick = static_cast<long long>(mTickCounter);
	outOwned = GetPoolObjectCount();
	outContacts = mContactsSinceReport;
	outMinX = (mServerBorderData != nullptr) ? mServerBorderData->minXVal : 0.0f;
	outMaxX = (mServerBorderData != nullptr) ? mServerBorderData->maxXVal : 0.0f;

	// This server's own border struct is NOT the one the repartition path updates -
	// that one lives in the border map - so read the map where it has an entry for us.
	if (mServerBorderMap != nullptr) {
		const auto mine = mServerBorderMap->find(mServerID);
		if (mine != mServerBorderMap->end() && mine->second != nullptr) {
			outMinX = mine->second->minXVal;
			outMaxX = mine->second->maxXVal;
		}
	}

	// Where the load sits along X inside this region.
	//
	// Approximated by OBJECT COUNT per bucket rather than by contacts per bucket: a
	// contact belongs to two objects that may be in different buckets, so attributing
	// it to one of them would be arbitrary. Object count within a bucket is a good
	// proxy because contact cost scales with local density - which is precisely what
	// a bucketed count measures - and it costs nothing to compute.
	if (outBuckets != nullptr) {
		for (int i = 0; i < LOAD_REPORT_BUCKETS; ++i) {
			outBuckets[i] = 0;
		}
		const float width = outMaxX - outMinX;
		if (width > 0.0f) {
			for (const auto& entry : mCreatedObjectPool) {
				if (entry.second == nullptr || entry.second->IsHaloShadow()) {
					continue;
				}
				const float x = entry.second->GetTransform().GetPosition().x;
				int bucket = static_cast<int>(((x - outMinX) / width)
					* static_cast<float>(LOAD_REPORT_BUCKETS));
				bucket = std::max(0, std::min(LOAD_REPORT_BUCKETS - 1, bucket));
				++outBuckets[bucket];
			}
		}
	}

	mContactsSinceReport = 0;
	mLastLoadReportTick = mTickCounter;
	return true;
}

void DistributedGameServer::ServerWorldManager::ScheduleOutgoingObject(int networkObjectID,
	int newOwnerServerID) {
	// With no lookahead the receiver applies on arrival, so there is no agreed tick to
	// wait for and holding on would only delay the transfer.
	if (mHandoffLookaheadTicks <= 0) {
		HandleOutgoingObject(networkObjectID, newOwnerServerID);
		return;
	}

	ScheduledRelease release;
	release.newOwnerServerID = newOwnerServerID;
	// The packet carried mSenderTick = this same tick counter, and the receiver
	// installs at mSenderTick + lookahead. Computing it the same way on both sides is
	// what makes the exchange atomic without an acknowledgement.
	release.releaseAtTick = mTickCounter + static_cast<uint64_t>(mHandoffLookaheadTicks);
	mScheduledReleases[networkObjectID] = release;
}

bool DistributedGameServer::ServerWorldManager::IsReleasePending(int networkObjectID) const {
	return mScheduledReleases.find(networkObjectID) != mScheduledReleases.end();
}

void DistributedGameServer::ServerWorldManager::FlushScheduledReleases() {
	if (mScheduledReleases.empty()) {
		return;
	}

	// std::map, so iteration is already in object-id order and two releases due on the
	// same tick always happen in the same order - the same total order
	// FlushScheduledHandoffs sorts for, obtained here for free.
	for (auto entry = mScheduledReleases.begin(); entry != mScheduledReleases.end(); ) {
		if (entry->second.releaseAtTick > mTickCounter) {
			++entry;
			continue;
		}
		const int objectID = entry->first;
		const int newOwner = entry->second.newOwnerServerID;
		entry = mScheduledReleases.erase(entry);
		HandleOutgoingObject(objectID, newOwner);
	}
}

void DistributedGameServer::ServerWorldManager::RecordPendingTransfer(
	const CSC8503::StartSimulatingObjectPacket& packet, int targetServerID) {
	PendingTransfer transfer;
	transfer.packet = std::make_unique<CSC8503::StartSimulatingObjectPacket>(packet);
	transfer.targetServerID = targetServerID;
	transfer.lastSentTick = mTickCounter;
	transfer.attempts = 1;
	mPendingTransfers[packet.objectID] = std::move(transfer);
}

bool DistributedGameServer::ServerWorldManager::PopHandoffResend(
	CSC8503::StartSimulatingObjectPacket& out) {
	while (!mHandoffResendQueue.empty()) {
		const int objectID = mHandoffResendQueue.front();
		mHandoffResendQueue.erase(mHandoffResendQueue.begin());
		const auto entry = mPendingTransfers.find(objectID);
		// Acked between being queued and being drained - nothing to resend.
		if (entry == mPendingTransfers.end() || entry->second.packet == nullptr) {
			continue;
		}
		out = *entry->second.packet;
		return true;
	}
	return false;
}

void DistributedGameServer::ServerWorldManager::FlushPendingTransfers() {
	if (mPendingTransfers.empty()) {
		return;
	}
	// std::map, so object-id order - two reclaims on the same tick always happen in
	// the same order, which is what keeps a run reproducible.
	for (auto entry = mPendingTransfers.begin(); entry != mPendingTransfers.end(); ) {
		const NCL::Distributed::CustodyAction action = NCL::Distributed::DecideCustody(
			mTickCounter, entry->second.lastSentTick, entry->second.attempts,
			mCustodyConfig);

		if (action == NCL::Distributed::CustodyAction::Wait) {
			++entry;
			continue;
		}

		if (action == NCL::Distributed::CustodyAction::Resend) {
			mHandoffResendQueue.push_back(entry->first);
			entry->second.lastSentTick = mTickCounter;
			++entry->second.attempts;
			++mHandoffsResent;
			++entry;
			continue;
		}

		// Reclaim. The sender applies its OWN transfer packet back to itself, which is
		// the same path an incoming handoff takes - correct precisely because
		// HandleOutgoingObject erased the pool entry rather than nulling it, so a
		// fresh construct is the normal arrival case and not a tombstone conflict.
		const int objectID = entry->first;
		CSC8503::StartSimulatingObjectPacket reclaimed = *entry->second.packet;
		reclaimed.newOwnerServerID = mServerID;
		entry = mPendingTransfers.erase(entry);

		// Cleared BEFORE the apply: while this entry stands, a command for the object
		// would be forwarded to a server that does not have it.
		mLastKnownOwner.erase(objectID);
		mLastKnownOwnerTick.erase(objectID);

		ApplyIncomingObject(&reclaimed, true);
		++mHandoffsReclaimed;
		std::cout << "Reclaimed unacknowledged handoff of object " << objectID << "\n";
	}
}

CSC8503::GameObject* DistributedGameServer::ServerWorldManager::PromoteHaloShadow(int networkID) {
	const auto entry = mHaloObjects.find(networkID);
	if (entry == mHaloObjects.end() || entry->second == nullptr) {
		return nullptr;
	}

	GameObject* object = entry->second;
	mHaloObjects.erase(entry);
	mHaloState.erase(networkID);

	// It stops being someone else's copy and becomes ours. Everything it needs to be
	// a first-class object it already has - it is in the GameWorld, registered with
	// the physics system, and carrying the right archetype and contact-order id.
	object->SetIsHaloShadow(false);

	auto* networkObject = new NetworkObject(*object, networkID);
	object->SetNetworkObject(networkObject);
	AddNetworkObjectToNetworkObjects(networkObject);
	mCreatedObjectPool[networkID] = object;

	// Deliberately NOT added to mTestObjects here. ApplyIncomingObject does that on
	// the shared path just below, for a constructed object and a promoted one alike;
	// adding it here too put every promoted object in the list twice, which inflated
	// the owned-object count by exactly the number of handoffs received.
	return object;
}

void DistributedGameServer::ServerWorldManager::CheckPositionOutOfServerBoundaries() {
	for (const auto& gameObj : mGameWorld->GetGameObjects()) {
		// A halo shadow is already outside this server's region by construction -
		// that is what makes it a shadow. Handing it off would mean offering a
		// neighbour an object it already owns.
		if (gameObj->IsHaloShadow()) {
			continue;
		}
		if (gameObj->HasPhysics() && gameObj->IsNetworkActive()) {
			if (auto* networkComp = gameObj->GetNetworkObject()) {
				// Already promised to a neighbour, and still ours until the agreed
				// tick. It is outside our region for that whole window, so without
				// this it would be re-detected and re-sent every tick.
				if (IsReleasePending(networkComp->GetNetworkID())) {
					continue;
				}
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

void DistributedGameServer::ServerWorldManager::DrainScheduledArrivals() {
	// The tick counter is frozen once the run has ended, so anything still queued was
	// scheduled for a tick that will never arrive. Applied unconditionally: the point
	// is to account for it, not to simulate it.
	for (auto& entry : mScheduledHandoffs) {
		ApplyIncomingObject(entry.packet.get());
	}
	mScheduledHandoffs.clear();
	FlushPendingDeletions();
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

bool DistributedGameServer::ServerWorldManager::ApplyIncomingObject(StartSimulatingObjectPacket* packet, bool isReclaim) {
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

	// Incoming handoffs that land outside the receiving region, observed but not
	// corrected: CalculateIncomingObjectOffsetPosition computes what a clamp would
	// do and the result is DISCARDED below. A reclaim lands outside this server's
	// region by construction (that is the whole point of a reclaim), so isReclaim
	// is excluded here or every reclaim would fire this and the counter would lose
	// its evidential value as a check on genuine handoff targeting.
	if (!isReclaim && mServerBorderData != nullptr) {
		const Maths::Vector3 incoming = packet->lastFullState.position;
		const Maths::Vector3 clamped = CalculateIncomingObjectOffsetPosition(incoming);
		if (clamped.x != incoming.x || clamped.z != incoming.z) {
			++mHandoffsClamped;
		}
	}

	// This server has been shadowing the object right up to the moment it became ours,
	// so the handoff is a PROMOTION rather than a construction: the shadow already
	// exists, is already in the physics system, and already has the object's contact
	// history. Tearing it down and building a replacement would throw that away, and
	// UpdateCollisionList carries contacts for several frames - an object mid-collision
	// at the border would have its contacts silently reset by crossing it.
	PromoteHaloShadow(packet->objectID);

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
		// We own it now. Recorded rather than erased so a stale relay that arrives
		// here is answered with "us" instead of falling through to ObjectUnknown.
		RecordObjectOwner(packet->objectID, mServerID);

		++mHandoffsReceived;

		std::cout << "Starting simulating object: " << packet->objectID << "/ Game world object count: " << mGameWorld->GetGameObjects().size() << "\n";
		return true;
	}

	++mHandoffsFailed;
	return false;
}

void DistributedGameServer::ServerWorldManager::HandleTransitionHandshakeReceived(
	CSC8503::StartSimulatingObjectReceivedPacket* packet) {
	if (packet == nullptr) {
		return;
	}
	// The ack discharges custody: the receiver has accepted the object and will
	// install it on the agreed tick, so this server is no longer responsible for it.
	mPendingTransfers.erase(packet->objectID);

	// Clearing mIsWaitingHandshake is only possible while the NetworkObject still
	// exists, and whether it does depends on the handoff lookahead:
	//
	//  - lookahead 0: the object was released - and TORN DOWN - on the tick it was
	//    sent, so by the time this ack arrives there is nothing left to call. Calling
	//    NetworkObject::OnTransitionHandshakeReceived() unguarded here would be a
	//    use-after-free.
	//  - lookahead > 0: release happens at senderTick + lookahead, far later than one
	//    round trip, so the object is still here and the flag can be cleared.
	//
	// Looking it up rather than assuming either case is what makes this safe in both.
	const auto poolEntry = mCreatedObjectPool.find(packet->objectID);
	if (poolEntry != mCreatedObjectPool.end() && poolEntry->second != nullptr) {
		if (auto* networkObject = poolEntry->second->GetNetworkObject()) {
			networkObject->OnTransitionHandshakeReceived();
		}
	}
}

void DistributedGameServer::ServerWorldManager::HandleOutgoingObject(int networkObjectID,
	int newOwnerServerID) {
	// Recorded before anything else, so it holds even on the error path below: a
	// command arriving after the release must still be forwardable.
	RecordObjectOwner(networkObjectID, newOwnerServerID);

	auto poolEntry = mCreatedObjectPool.find(networkObjectID);
	if (poolEntry == mCreatedObjectPool.end()) {
		std::cout << "ERROR: outgoing handoff for unknown object id " << networkObjectID << "\n";
		return;
	}
	if (auto* gameObj = poolEntry->second) {
		std::cout << "Removing object from server with network ID" << networkObjectID << "\n";
		// Torn down, not just deactivated. Leaving a deactivated copy behind is what
		// made per-server state O(world): an object handed away stayed allocated here
		// forever. The pool entry is ERASED rather than nulled - unlike a destroy,
		// this object still exists, it just lives somewhere else now, so a later
		// arrival must be able to construct it (a null entry would be read as a
		// tombstone). mLastKnownOwner, written above, is what remains of it here.
		//
		// The actual free is deferred to FlushPendingDeletions at the end of the tick:
		// the collision sets hold raw pointers to this object for several frames.
		TeardownObject(gameObj);
		mCreatedObjectPool.erase(poolEntry);
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

			// Index, id and archetype are derived for EVERY cell, owned or not.
			//
			// This loop is the shared naming of the world: mNetworkIdBuffer advances
			// in grid order and is the ONLY reason ids agree across servers. Skipping
			// an iteration - rather than skipping construction within an iteration -
			// would shift every subsequent id and silently desynchronise the entire id
			// space, which would surface much later as handoffs for objects that do
			// not exist. Gate the construction, never the iteration.
			const int objectIndex = objCounter++;
			const int networkId = mNetworkIdBuffer++;

			const bool isCube = (DeterministicHash(mWorldSeed, playerID, objectIndex) & 1u) != 0;
			const int archetypeID = static_cast<int>(isCube
				? NCL::Interaction::ObjectArchetype::Cube
				: NCL::Interaction::ObjectArchetype::Sphere);

			const int owner = GetObjectServer(objPos);

			if (owner == mServerID) {
				Transform transform;
				transform.SetPosition(objPos);

				GameObject* obj = isCube
					? AddCubeToWorld(transform, objectIndex, playerID)
					: AddSphereToWorld(transform, objectIndex, playerID);

				// Explicit id rather than AddNetworkObject's counter side effect: the
				// counter is advanced above for every cell, so taking it here as well
				// would double-count.
				auto* networkObject = new NetworkObject(*obj, networkId);
				obj->SetNetworkObject(networkObject);
				AddNetworkObjectToNetworkObjects(networkObject);

				mCreatedObjectPool[networkId] = obj;
				// Recorded for pre-seeded objects too: without it a late joiner would
				// be told every existing object is the default archetype.
				mObjectArchetypes[networkId] = archetypeID;

				ApplyWorkloadInitialState(*obj, playerID, objectIndex);

				mTestObjects.push_back(dynamic_cast<TestObject*>(obj));
				mGameWorld->AddGameObject(obj);
			}
			else if (owner >= 0) {
				// Not ours, and nothing at all is recorded - not an object, not even a
				// forwarding entry. An 8-byte entry per non-owned cell sounds cheap
				// until the world is the size this system is for: it is O(world) per
				// server, the same order as the pre-seed model this increment removed,
				// just with a smaller constant. A command that arrives here for this
				// object is forwarded using the position the client stamped on it
				// (CommandFlags::HasObjectPosition); the forwarding table is now only
				// for objects this server has actually handed away.
			}
			else {
				// Outside every region. Previously such an object was built on every
				// server and left deactivated, so it existed everywhere and was
				// simulated nowhere; now it exists nowhere. Loud, because it means the
				// grid extends past the world bounds.
				std::cout << "WARNING: pre-seed cell " << networkId << " at " << objPos
					<< " maps to no server; not created.\n";
			}

			if (objectsPerPlayer == objCounter) {
				Profiler::SetTotalObjectsInServer(mNetworkIdBuffer - NETWORK_ID_BUFFER);
				return;
			}
		}
	}

	// objPreseed counts ids ALLOCATED, not objects built here, so it stays the
	// world-wide total and remains comparable across servers and across this change.
	Profiler::SetTotalObjectsInServer(mNetworkIdBuffer - NETWORK_ID_BUFFER);
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
	if (mRegionsDirty || mCachedRegions.size() != mapSize) {
		mRegionsDirty = false;
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
