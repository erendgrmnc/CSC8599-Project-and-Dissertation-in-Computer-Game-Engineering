#pragma once

#include <memory>
#include <set>
#include <string>

#include "DistributedSystemCommonFiles/MetricSink.h"
#include "DistributedSystemCommonFiles/InteractionCommand.h"
#include "DistributedSystemCommonFiles/RegionOwnership.h"
#include "DistributedSystemCommonFiles/NetworkIdSpace.h"

namespace NCL::CSC8503 {
	struct StartSimulatingObjectReceivedPacket;
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

		// Borders are computed as doubles by GameInstance::CalculateServerBorders and
		// serialised as text. These were ints, so any world extent not divisible by
		// the row/column count truncated on arrival - opening gaps and overlaps
		// between regions. Objects landing in a gap map to server -1 and never hand
		// off. Keep these floating point so the server's regions match the manager's.
		struct PhysicsServerBorderData {
			float maxZVal;
			float minZVal;

			float maxXVal;
			float minXVal;
		};

		// Implements ICommandContext so interaction commands can act on the world
		// without ever seeing the network layer, and vice versa.
		class ServerWorldManager : public NCL::Interaction::ICommandContext {
		public:
			ServerWorldManager(int serverID, PhysicsServerBorderData& physcisServerBorderData, std::map<const int, PhysicsServerBorderData*>& map);
			NCL::CSC8503::GameWorld* GetGameWorld() const;

			CSC8503::GameObject* AddDistributedControllableObject(const CSC8503::Transform& transform,int playerID) const;
			CSC8503::GameObject* AddCubeToWorld(const CSC8503::Transform& transform, int count, int playerID) const;
			CSC8503::GameObject* AddSphereToWorld(const CSC8503::Transform& transform, int count, int playerID) const;
			CSC8503::GameObject* AddFloorWorld(const CSC8503::Transform& transform);

			bool StartHandlingObject(CSC8503::StartSimulatingObjectPacket* packet);

			void Update(float dt);
			void AddNetworkObject(CSC8503::GameObject& objToAdd);
			void CreatePlayerObjects(int playerCount, int objectsPerPlayer);
			void HandleTransitionHandshakeReceived(CSC8503::StartSimulatingObjectReceivedPacket* packet);
			void HandleOutgoingObject(int networkObjectID);
			void CreateObjectGrid(int rowCount, int colCount, int objectsPerPlayer, float rowSpacing, float colSpacing, int playerID, const Maths::Vector3& startPos);

			std::vector<CSC8503::TestObject*> GetTestObjects();
			std::vector<CSC8503::NetworkObject*>* GetNetworkObjects();

			// Seed for the deterministic world construction. Every server building an
			// instance must use the same value or their object sets diverge.
			void SetWorldSeed(unsigned int seed) {
				mWorldSeed = seed;
			}

			unsigned int GetWorldSeed() const {
				return mWorldSeed;
			}

			// Pins the physics substep rate. Required for reproducible measurement -
			// see PhysicsSystem::SetFixedTimestep.
			void SetFixedTimestep(bool state);

			// The pinned substep length, in seconds. Feeding this to the headless loop
			// as its dt makes each tick perform exactly one substep, which is what
			// turns a run deterministic.
			float GetFixedTimestepDt() const;

			// --- ICommandContext ---
			int GetServerID() const override;
			int GetOwningServer(const Maths::Vector3& worldPoint) const override;
			CSC8503::GameObject* FindActiveObject(int networkObjectID) const override;
			bool TryGetLastKnownPosition(int networkObjectID, Maths::Vector3& out) const override;
			int SpawnObject(int archetypeID, const Maths::Vector3& at, int spawnerPlayerID) override;
			bool DestroyObject(int networkObjectID, NCL::Interaction::DespawnReason reason,
				int destroyerPlayerID) override;
			void ApplyImpulse(int networkObjectID, const Maths::Vector3& impulse) override;
			void ApplyRadialImpulse(const Maths::Vector3& origin, float radius, float magnitude) override;
			void SetMoveAxis(int networkObjectID, int playerID, const Maths::Vector3& axis) override;
			void RelayToServer(int serverID, NCL::Interaction::CommandType type,
				const NCL::Interaction::CommandArgs& args) override;
			void GetOverlappedServers(const Maths::Vector3& origin, float radius,
				std::vector<int>& outServerIDs) const override;

			// Relays are queued rather than sent, so a command never re-enters the
			// network layer from inside a packet handler. The manager drains this
			// after Apply returns.
			struct PendingRelay {
				int targetServerID = -1;
				NCL::Interaction::CommandType type = NCL::Interaction::CommandType::None;
				NCL::Interaction::CommandArgs args;
			};
			bool PopPendingRelay(PendingRelay& out);

			// Same queue-and-drain shape as relays: SpawnObject builds the object but
			// cannot broadcast it, because the world manager has no network access.
			struct PendingSpawn {
				int objectID = -1;
				int archetypeID = 0;
				int ownerServerID = -1;
				int spawnerPlayerID = -1;
				Maths::Vector3 position;
			};
			bool PopPendingSpawn(PendingSpawn& out);

			// Builds the DEACTIVATED twin of an object spawned on a peer. This is the
			// whole reason spawn needs a broadcast: StartHandlingObject requires the
			// target server to already hold a pool entry, so a runtime spawn has to
			// reproduce the pre-seed model on every server rather than exist only on
			// its owner. Returns false if the id is already known.
			bool CreateReplicatedSpawn(int networkID, int archetypeID, int ownerServerID,
				int spawnerPlayerID, const Maths::Vector3& position);

			struct PendingDespawn {
				int objectID = -1;
				int reason = 0;
				int destroyerPlayerID = -1;
			};
			bool PopPendingDespawn(PendingDespawn& out);

			// Applies a destroy that originated on a peer. Idempotent: a second
			// despawn for the same id observes the tombstone and is ignored.
			void ApplyRemoteDespawn(int networkID, int reason, int destroyerPlayerID);

			// One manifest entry per object this server currently owns, for a late
			// joiner. Archetype is carried so the joiner builds the right shape rather
			// than inferring a default from a transform-only snapshot.
			struct ManifestEntry {
				int objectID = -1;
				int archetypeID = 0;
				Maths::Vector3 position;
			};
			std::vector<ManifestEntry> BuildOwnedObjectManifest() const;

			bool IsTombstoned(int networkID) const {
				return mTombstones.find(networkID) != mTombstones.end();
			}

			// Selects the initial-motion workload applied when the world is built.
			//   ""        - none (default): objects fall and settle, never crossing a
			//               region border, so the handoff path is never exercised
			//   "shuttle" - deterministic lateral velocity, so objects traverse the
			//               world and cross borders at a measurable rate
			// Must be identical on every server: they each build the same object set
			// independently, so a workload mismatch desynchronises the world.
			// Fault injection: delays each outgoing handoff by N ticks while the object
			// is already released. Real links reorder rarely enough that race W3 (a
			// destroy reaching the new owner BEFORE the object does) and the client's
			// resurrection guard were never reached in ordinary runs - so both held
			// but were untested. This widens the window deterministically.
			//
			// Measurement runs must leave this at 0: it deliberately changes handoff
			// timing.
			void SetHandoffDelayTicks(int ticks) {
				mHandoffDelayTicks = ticks;
			}

			int GetHandoffDelayTicks() const {
				return mHandoffDelayTicks;
			}

			void SetWorkload(const std::string& workload) {
				mWorkload = workload;
			}

			const std::string& GetWorkload() const {
				return mWorkload;
			}

			// Handoff parity counters (invariant I5): every StartSimulatingObjectPacket
			// this server sends should be matched by exactly one successful
			// StartHandlingObject somewhere. A drift between these totals across all
			// servers means objects are being lost or duplicated in transit.
			int GetHandoffsSent() const {
				return mHandoffsSent;
			}

			int GetHandoffsReceived() const {
				return mHandoffsReceived;
			}

			int GetHandoffsFailed() const {
				return mHandoffsFailed;
			}

			void RecordHandoffSent() {
				++mHandoffsSent;
			}

			// Enables per-tick metric recording to a CSV. Empty path disables it, in
			// which case Record() is a no-op and nothing is allocated.
			void EnableMetrics(const std::string& outputPath, size_t capacity);

			// Writes any buffered samples out. Called on a clean shutdown; a run that
			// is force-killed loses whatever has not been flushed.
			void FlushMetrics();
		protected:
			int mNetworkIdBuffer;
			int mServerID;
			unsigned int mWorldSeed = 1u;
			std::string mWorkload;
			std::unique_ptr<NCL::MetricSink> mMetrics;
			uint64_t mTickCounter = 0;
			int mHandoffsSent = 0;
			int mHandoffsReceived = 0;
			int mHandoffsFailed = 0;
			int mHandoffDelayTicks = 0;
			double mPhysicsTime = 0;
			float mObjDebugTimer = 5.f;

			std::vector<NCL::CSC8503::NetworkObject*> mNetworkObjects;
			std::vector<NCL::CSC8503::TestObject*> mTestObjects;

			NCL::CSC8503::GameWorld* mGameWorld;
			NCL::CSC8503::PhysicsSystem* mPhysics;
			NCL::DistributedGameServer::PhysicsServerBorderData* mServerBorderData;

			std::map<int, NCL::CSC8503::GameObject*> mCreatedObjectPool;
			std::vector<PendingRelay> mPendingRelays;
			std::vector<PendingSpawn> mPendingSpawns;

			// Per-server counter feeding NetworkIdSpace::MakeRuntimeId. Static bit
			// partitioning means no server can ever mint another's id, so no central
			// allocator and no round trip per spawn.
			int mRuntimeSpawnCounter = 0;

			// objectID -> archetype, so a manifest can report what each object IS.
			// Pre-seeded objects are recorded too, otherwise a late joiner would get
			// the default shape for most of the world.
			std::map<int, int> mObjectArchetypes;

			std::vector<PendingDespawn> mPendingDespawns;

			// Destroyed ids, kept forever. IDs are never recycled (that would need
			// distributed agreement on when every server AND client has retired one -
			// a distributed GC problem), which is exactly what makes a permanent
			// tombstone safe and cheap.
			std::set<int> mTombstones;

			// A destroy can arrive at the new owner BEFORE the object does (race W3).
			// Dropping it would resurrect the object, so it is held here and applied
			// when StartHandlingObject later runs for that id.
			std::set<int> mPendingDestroyOnArrival;

			// Objects awaiting deletion. Never deleted on the tick they are destroyed:
			// UpdateCollisionList dereferences raw GameObject* for several frames
			// afterwards, so the pointer must outlive the collision purge.
			std::vector<CSC8503::GameObject*> mPendingDeletion;
			// Re-applies each controlled object's movement axis every tick. Continuous
			// input is state: the client sends the axis once and it stays in effect
			// until superseded, so applying it only on receipt would make movement
			// depend on the packet rate rather than on the input.
			void ApplyControlForces();

			void FlushPendingDeletions();
			void TeardownObject(CSC8503::GameObject* object);

			// Shared by the owner path and the peer-replica path so both build a
			// byte-identical object; only their active state differs.
			CSC8503::GameObject* CreateObjectFromArchetype(int archetypeID,
				const Maths::Vector3& position, int networkID, int playerID);

			// GetObjectServer runs once per object per tick, so the region list is
			// cached rather than rebuilt per call. The border map is populated after
			// construction (when the manager's start packet arrives), hence the lazy
			// rebuild keyed on its size rather than a one-shot copy.
			mutable std::vector<NCL::Interaction::RegionBounds> mCachedRegions;
			const std::vector<NCL::Interaction::RegionBounds>& GetRegionBounds() const;
			std::map<const int, PhysicsServerBorderData*>* mServerBorderMap;

			void AddNetworkObjectToNetworkObjects(NCL::CSC8503::NetworkObject* networkObj);
			void CheckPositionOutOfServerBoundaries();

			bool IsObjectInBorder(const Maths::Vector3& objectPosition) const;

			int GetObjectServer(const Maths::Vector3& position) const;

			Maths::Vector3 CalculateIncomingObjectOffsetPosition(const Maths::Vector3& position) const;

			void ApplyWorkloadInitialState(CSC8503::GameObject& obj, int playerID, int objectIndex) const;
		};
	}
}
