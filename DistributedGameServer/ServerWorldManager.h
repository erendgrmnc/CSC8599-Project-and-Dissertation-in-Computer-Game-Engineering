#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "DistributedSystemCommonFiles/MetricSink.h"
#include "DistributedSystemCommonFiles/InteractionCommand.h"
#include "DistributedSystemCommonFiles/RegionOwnership.h"
#include "DistributedSystemCommonFiles/NetworkIdSpace.h"
#include "DistributedSystemCommonFiles/HandoffCustody.h"

namespace NCL::CSC8503 {
	struct StartSimulatingObjectReceivedPacket;
	struct HaloObjectState;
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
			~ServerWorldManager();
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
			// newOwnerServerID is recorded so a command that arrives here after the
			// handoff can still be forwarded. Without it, releasing the object leaves
			// this server unable to say where it went and race W2 reopens.
			void HandleOutgoingObject(int networkObjectID, int newOwnerServerID);

			// Schedules the release for the SAME tick the receiver installs the object
			// on, instead of releasing the moment the packet is sent.
			//
			// Releasing on send opened an ownership gap the width of the handoff
			// lookahead: the receiver applies at senderTick + lookahead, so with the
			// 300-tick lookahead a reproducible run uses, every transferred object was
			// simulated by NOBODY for 2.5 seconds. Measured on a 200-object uniform
			// run, 1,397 of 1,800 ticks had at least one unowned object and as many as
			// 33 were unowned at once.
			//
			// Both sides act on the same agreed tick, so ownership changes atomically
			// with no barrier and no acknowledgement.
			void ScheduleOutgoingObject(int networkObjectID, int newOwnerServerID);

			// True while a release is scheduled but not yet due. The border check must
			// skip such an object: it is outside this server's region for the whole
			// pending window, so without this it would be re-detected and re-sent
			// every tick until the release finally fired.
			bool IsReleasePending(int networkObjectID) const;

			// Transfers initiated but not yet released, at the moment this is read.
			//
			// Since ownership transfers on an agreed tick rather than on send, hoSent
			// counts the START of a transfer and hoRecv its COMPLETION. A run that ends
			// mid-transfer therefore has hoSent > hoRecv legitimately, and the handoff
			// parity invariant has to subtract these rather than treat the difference
			// as a lost object. The object is not lost: the sender still owns it, which
			// is why conservation and the per-tick ownership check both stay exact.
			// Transfers that have ARRIVED but are waiting for their scheduled tick.
			//
			// The mirror of GetPendingReleaseCount on the receiving side. hoSent counts
			// a transfer when the sender releases it and hoRecv when the receiver
			// installs it, so a run ending between those two moments is short by
			// however many are queued at each end. Without this the difference reads as
			// lost objects: a 4,000-object migration ended with 3,381 received and 620
			// unaccounted, and the 620 were sitting right here.
			// Installs any arrival whose scheduled tick has passed, without stepping the
			// world. Used by the post-run drain: a transfer that lands after this
			// server's last tick must still be accounted for, or it reads as a lost
			// object when it is really a harness artefact.
			void DrainScheduledArrivals();

			int GetScheduledHandoffCount() const {
				return static_cast<int>(mScheduledHandoffs.size());
			}

			int GetPendingReleaseCount() const {
				return static_cast<int>(mScheduledReleases.size());
			}

			// --- dynamic repartitioning ---
			//
			// Queues a new partition for adoption at an ABSOLUTE tick. Every server
			// must switch on the same simulated tick: while two disagree about where a
			// border is, OwningServerFor gives different answers on each and an object
			// is owned by both of them or by neither.
			//
			// One region per server. A region for an id this server has never heard of
			// is inserted; the structs themselves are overwritten in place rather than
			// replaced, because ServerWorldManager and TestObject both hold references
			// taken from them at construction.
			struct PendingPartition {
				long long effectiveTick = 0;
				std::vector<NCL::Interaction::RegionBounds> regions;
			};
			void SchedulePartitionChange(const PendingPartition& partition);

			// Partitions that arrived after the tick they were meant to take effect on.
			// Adopted immediately anyway - a server left on a partition nobody else is
			// using cannot recover - but counted, because it means the run is not
			// reproducible and I1 may have been violated in the interval.
			int GetRepartitionsLate() const {
				return mRepartitionsLate;
			}

			int GetRepartitionCount() const {
				return mRepartitionCount;
			}
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

			// Worker threads for the parallel physics phases. 0 keeps everything on
			// the server's own thread, which is how every measurement before this ran.
			// Negative means "pick a sensible default from the hardware".
			void SetPhysicsWorkerThreads(int workerCount);

			// --- load reporting ---
			//
			// Every this many ticks the server reports its cost to the manager, which
			// may move the borders. 0 disables reporting, which is how every run before
			// dynamic rebalancing behaved.
			//
			// A TICK schedule, not a wall-clock one: the manager decides using reports
			// for one specific tick, so the reports themselves have to be emitted on a
			// tick the servers agree on, or two runs would balance at different points
			// in the simulation.
			void SetLoadReportInterval(int ticks) {
				mLoadReportIntervalTicks = ticks;
			}

			// True on the tick a report is due; fills the cost accumulated since the
			// last one and resets it. The caller sends the packet, because the world
			// manager never touches the network layer.
			// outBuckets must have room for LOAD_REPORT_BUCKETS entries: WHERE the load
			// is along X, not just how much. A total alone cannot locate a cluster
			// inside a region, so a manager working from totals cannot place a border.
			static constexpr int LOAD_REPORT_BUCKETS = 8;
			bool TakeLoadReport(long long& outTick, int& outOwned, long long& outContacts,
				float& outMinX, float& outMaxX, int* outBuckets);

			// The pinned substep length, in seconds. Feeding this to the headless loop
			// as its dt makes each tick perform exactly one substep, which is what
			// turns a run deterministic.
			float GetFixedTimestepDt() const;

			// --- ICommandContext ---
			int GetServerID() const override;
			int GetOwningServer(const Maths::Vector3& worldPoint) const override;

			// Union of every region: the world's outer bounds. False before the border
			// map has arrived from the manager.
			bool GetWorldExtent(float& minX, float& maxX, float& minZ, float& maxZ) const;
			CSC8503::GameObject* FindActiveObject(int networkObjectID) const override;
			bool TryGetLastKnownPosition(int networkObjectID, Maths::Vector3& out) const override;
			bool TryGetLastKnownOwner(int networkObjectID, int& outServerID) const override;

			// Records where an object went. Called when this server hands one away,
			// when it learns of a spawn owned elsewhere, and for every pre-seed grid
			// cell outside its own region.
			void RecordObjectOwner(int networkObjectID, int serverID);
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

			// Notes that a peer spawned an object, WITHOUT building anything. It used
			// to build a deactivated twin because StartHandlingObject could only
			// reactivate an object the server already held; handoff now constructs on
			// arrival, so all a non-owner needs is somewhere to forward commands.
			// Returns false if this server already holds the object active - the spawn
			// broadcast lost a race with a handoff of the same object to us, and the
			// stale owner in it must not be recorded.
			bool RecordRemoteSpawn(int networkID, int ownerServerID);

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
			//   "shuttle" - deterministic lateral velocity from ONE start region, so
			//               objects traverse the world and cross borders. Adversarial
			//               for a static partition: it starts ~90% loaded on one server
			//   "uniform" - same motion, but the starting grid is spread across the
			//               whole world. The balanced counterpart to shuttle, and the
			//               fair speedup case
			//   "seam"    - grid centred on the origin so a whole row and column sit
			//               exactly on the region borders
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
			uint64_t GetTickCounter() const {
				return mTickCounter;
			}

			// Schedules an incoming handoff for a deterministic tick instead of
			// applying it the instant the packet lands. 0 keeps the original
			// apply-on-arrival behaviour.
			void SetHandoffLookaheadTicks(int ticks) {
				mHandoffLookaheadTicks = ticks;
			}

			int GetHandoffLookaheadTicks() const {
				return mHandoffLookaheadTicks;
			}

			// Handoffs that arrived too late to hit their scheduled tick. Non-zero
			// means the lookahead is too small for the link, and that the run is not
			// reproducible - so it is reported rather than silently absorbed.
			int GetHandoffsLate() const {
				return mHandoffsLate;
			}

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

			void RecordPendingTransfer(const CSC8503::StartSimulatingObjectPacket& packet,
				int targetServerID, int batchSize = 1);
			bool PopHandoffResend(CSC8503::StartSimulatingObjectPacket& out);
			// Feeds the network layer's verdict back into custody. Called by
			// DistributedGameServerManager for every packet it drains from
			// PopHandoffResend: `peerLinkGone` means the resend could not even be
			// attempted because no peer link to that server exists, and it is the ONLY
			// evidence that authorises a reclaim. Deliberately NOT "the send returned
			// false" - ENet also refuses when the outgoing queue is full, which is
			// overload, not death. This exists so that ServerWorldManager still never
			// touches the network layer itself - same idiom as
			// PendingRelay/PopPendingRelay.
			void RecordHandoffResendResult(int objectID, bool peerLinkGone);
			// retryTicks is a COUNT OF TICKS on the CLI and in the config struct's name,
			// but mCustodyConfig.retryTicks below actually holds MICROSECONDS: the
			// conversion is NCL::Distributed::CustodyTicksToMicros (HandoffCustody.h),
			// pulled out to a pure, unit-tested function rather than left inline here -
			// see that function's comment for why a raw tick count is not safe to
			// compare across two servers, and Task 6a's report for the measurement that
			// caught it. FlushPendingTransfers is what actually compares this value,
			// against NCL::MonotonicMicros(), further adjusted per-transfer by
			// NCL::Distributed::ScaleCustodyRetryMicros.
			void SetCustodyConfig(int retryTicks, int maxAttempts) {
				const float dt = GetFixedTimestepDt();
				const double nominalDt = (dt > 0.0f) ? static_cast<double>(dt) : (1.0 / 120.0);
				mCustodyConfig.retryTicks = static_cast<int>(
					NCL::Distributed::CustodyTicksToMicros(retryTicks, nominalDt, 2000000000));
				mCustodyConfig.maxAttempts = maxAttempts;
			}
			int GetHandoffsResent() const {
				return mHandoffsResent;
			}
			int GetHandoffsReclaimed() const {
				return mHandoffsReclaimed;
			}
			int GetPendingCustodyCount() const {
				return static_cast<int>(mPendingTransfers.size());
			}
			int GetHandoffsClamped() const {
				return mHandoffsClamped;
			}
			// Redundant arrivals suppressed by the idempotence guard in
			// ApplyIncomingObject. Deliberately its OWN counter and NOT folded into
			// mHandoffsReceived: hoSent is only incremented for the original send, so
			// counting a duplicate arrival into hoRecv would break handoff parity (I5)
			// by exactly the duplicate count. See the guard's comment.
			int GetHandoffsDuplicate() const {
				return mHandoffsDuplicate;
			}

			// Locality counters (invariant I6). What this server HOLDS, as distinct
			// from what it simulates. Under the pre-seed model every server
			// instantiates the whole world and deactivates what it does not own, so
			// both of these equal the world total on every server while the owned
			// count is only its region's share. The region-local increment is the
			// claim that these stop scaling with world size.
			int GetPoolObjectCount() const {
				return static_cast<int>(mCreatedObjectPool.size());
			}

			int GetWorldObjectCount() const;

			// The forwarding table. Reported alongside the pool because it is the
			// other thing that could quietly scale with the world: it was once
			// pre-populated with every object this server does not own, which is
			// O(world) per server dressed up as a small constant. It now only holds
			// objects this server has actually handed away, so it scales with handoff
			// traffic, not world size.
			int GetForwardEntryCount() const {
				return static_cast<int>(mLastKnownOwner.size());
			}

			// Halo shadows: read-only copies of objects owned by a NEIGHBOURING server,
			// held so that objects either side of a border can collide. Reported
			// separately from objPool rather than folded into it, because the two
			// answer different questions - objPool is what this server is responsible
			// for, objHalo is what it is merely watching. Adding them would make the
			// locality measurement (I6) unreadable.
			int GetHaloObjectCount() const {
				return static_cast<int>(mHaloObjects.size());
			}

			// --- halo band publication ---
			//
			// The band width. An object within this distance of a NEIGHBOUR's region
			// has to be visible to that neighbour, or a contact that happens across
			// the border is seen by nobody.
			//
			// Not a free tuning parameter: it has a hard floor. A halo update is
			// applied at senderTick + lookahead, so between sampling and application
			// an object can travel v_max * lookahead * dt. If the band is narrower
			// than that plus both radii, an object can go from outside the band to in
			// contact without ever having been published, and the contact is missed.
			// SetHaloWidth warns when it is set below the floor rather than silently
			// accepting it - the symptom otherwise is occasional missed contacts that
			// vary with load, which is close to undiagnosable.
			void SetHaloWidth(float width);

			void SetHaloLookaheadTicks(int ticks) {
				mHaloLookaheadTicks = ticks;
			}

			int GetHaloLookaheadTicks() const {
				return mHaloLookaheadTicks;
			}

			float GetHaloWidth() const {
				return mHaloWidth;
			}

			// The floor described above, for the current lookahead and substep.
			float MinimumSafeHaloWidth() const;

			// Fills `out` with (targetServerID, state) for every owned object that is
			// within the band of some other server's region. One object can appear
			// more than once: near a corner on a 2x2 grid it is within the band of
			// two neighbours and must be published to both.
			//
			// Uses GetOverlappedServers - the same query area effects use - rather
			// than a second, separately written border test. Two border tests that
			// disagree is precisely the bug the ownership unification fixed.
			struct HaloPublication {
				int targetServerID = -1;
				int objectID = -1;
			};
			void CollectHaloPublications(std::vector<HaloPublication>& out) const;

			// Fills `state` from the object's live transform and physics. False if the
			// object is not one this server owns.
			bool TryGetHaloState(int objectID, CSC8503::HaloObjectState& state) const;

			// Queues one neighbour's object state for application at
			// senderTick + halo lookahead, or applies it immediately and counts it as
			// late if that tick has already passed. Mirrors StartHandlingObject.
			void ScheduleHaloUpdate(const CSC8503::HaloObjectState& state, int senderTick,
				int senderServerID);

			int GetHaloUpdatesLate() const {
				return mHaloUpdatesLate;
			}

			// What an object IS, for a handoff packet to carry. Recorded for
			// pre-seeded objects as well as runtime spawns, so this answers for every
			// object this server knows. Falls back to Cube for an unknown id: a
			// handoff that could not name a shape would be unbuildable on arrival,
			// and losing the object is worse than getting its shape wrong.
			int GetObjectArchetype(int networkID) const {
				const auto entry = mObjectArchetypes.find(networkID);
				return (entry == mObjectArchetypes.end())
					? static_cast<int>(NCL::Interaction::ObjectArchetype::Cube)
					: entry->second;
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
			int mHandoffLookaheadTicks = 0;
			int mHandoffsLate = 0;

			// Handoffs waiting for their scheduled tick. Buffered by value: the packet
			// is a POD copy, so nothing here depends on the network buffer surviving.
			// Held by pointer, not value: this header only forward-declares the packet
			// (including NetworkObject.h here would drag the whole USEGL-guarded
			// networking layer into every translation unit that touches the world).
			// The out-of-line destructor below is what lets unique_ptr work with an
			// incomplete type here.
			struct ScheduledHandoff {
				std::unique_ptr<CSC8503::StartSimulatingObjectPacket> packet;
				uint64_t applyAtTick = 0;
			};
			std::vector<ScheduledHandoff> mScheduledHandoffs;
			void FlushScheduledHandoffs();

			// A transfer that has been SENT but not yet acknowledged.
			//
			// Simulation ownership is unaffected: HandleOutgoingObject still tears the
			// object down on exactly the tick it always did. What this adds is that the
			// sender does not FORGET the transfer. HandleOutgoingObject erases the pool
			// entry, so without this record an object the receiver never installed
			// exists nowhere - which is how E7 lost objects past the tick budget.
			//
			// unique_ptr rather than by value because this header only forward-declares
			// StartSimulatingObjectPacket, matching ScheduledHandoff above.
			//
			// Despite the name, lastSentTick holds a NCL::MonotonicMicros() wall-clock
			// timestamp, not a value of mTickCounter - see SetCustodyConfig for why a
			// simulation tick count is not safe to compare across two servers whose tick
			// rates can differ by two orders of magnitude.
			// batchSizeAtSend is how many OTHER handoffs were sent to a peer in the same
			// tick as this one - see RecordPendingTransfer. A receiver asked to accept
			// several handoffs at once applies and acknowledges them one at a time, so a
			// transfer sent as part of a 50-object burst genuinely needs on the order of
			// 50x the grace a lone transfer would; a fixed per-transfer deadline cannot
			// tell the two apart; a deadline scaled by however many the receiver was
			// simultaneously handed can.
			// peerLinkGone is the ONLY thing that may trigger a reclaim - see
			// NCL::Distributed::DecideCustody. It is fed back from
			// DistributedGameServerManager (which owns the network layer) via
			// RecordHandoffResendResult: a resend that failed with no peer link to that
			// server is evidence the peer is gone, as opposed to a deadline expiring,
			// which only ever meant "slow".
			struct PendingTransfer {
				std::unique_ptr<CSC8503::StartSimulatingObjectPacket> packet;
				int targetServerID = -1;
				uint64_t lastSentTick = 0;
				int attempts = 1;
				int batchSizeAtSend = 1;
				bool peerLinkGone = false;
			};
			// Keyed by object id: a resend must replace, never duplicate, the record.
			std::map<int, PendingTransfer> mPendingTransfers;
			void FlushPendingTransfers();
			// Object ids whose transfer needs re-sending. Drained by
			// DistributedGameServerManager, which owns the network layer.
			std::vector<int> mHandoffResendQueue;
			NCL::Distributed::CustodyConfig mCustodyConfig;
			int mHandoffsResent = 0;
			int mHandoffsReclaimed = 0;
			int mHandoffsDuplicate = 0;

			// Incoming handoffs that landed OUTSIDE the receiving region.
			//
			// CalculateIncomingObjectOffsetPosition exists to nudge such an object
			// back inside but has never been called. Since ownership was unified
			// behind OwningServerFor(), a handoff target is computed from the
			// transmitted position, so an arrival should be in-region by
			// construction and the function should be dead code for a good reason
			// rather than by accident. This counts how often that is untrue. The
			// clamp is computed and DISCARDED - never applied - so this observes
			// without changing behaviour.
			int mHandoffsClamped = 0;

			// The part of StartHandlingObject that actually installs the object.
			bool ApplyIncomingObject(CSC8503::StartSimulatingObjectPacket* packet, bool isReclaim = false);
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

			// objectID -> the server this one last believed owned it. Replaces the job
			// the deactivated twin was doing for command forwarding: a server that has
			// handed an object away keeps 8 bytes saying where it went, instead of a
			// whole GameObject. Entries are pure cache - a miss falls back to the
			// client's own owner table - so this can be bounded or dropped if it ever
			// needs to be. Only objects that have actually passed through this server
			// appear here, so it does not reintroduce an O(world) cost.
			std::map<int, int> mLastKnownOwner;
			// Tick each forwarding entry was last written or used. The table is what
			// lets a command chase an object this server has handed away, and an entry
			// is only useful while the client's view can still be that stale - a few
			// seconds. Without an age it grew monotonically with cumulative handoff
			// traffic and was never pruned, so a long run leaked one entry per object
			// ever touched.
			std::map<int, uint64_t> mLastKnownOwnerTick;
			void PruneForwardingTable();

			// Deliberately NOT in mCreatedObjectPool. A shadow is not this server's
			// object, and keeping it out of the pool is what makes FindActiveObject,
			// the snapshot loop and the handoff path skip it without needing a guard
			// in each - the ones that iterate the GameWorld instead do need the
			// explicit IsHaloShadow() test.
			std::map<int, CSC8503::GameObject*> mHaloObjects;
			// 0 disables publication entirely, which is what every measurement taken
			// before this increment ran with.
			float mHaloWidth = 0.0f;
			int mHaloUpdatesLate = 0;

			// The authoritative state of each shadow, as its owner last told us.
			// Re-imposed at the top of every tick, because contact resolution writes
			// position AND velocity straight into both bodies - skipping the
			// integrator is not enough to make a shadow read-only.
			struct HaloAuthoritativeState {
				Maths::Vector3 position;
				Maths::Vector3 linearVelocity;
				Maths::Vector3 angularVelocity;
				Maths::Quaternion orientation;
				int ownerServerID = -1;
				// The SENDER's tick when this state was sampled. Not the tick it was
				// applied on: the shadow is extrapolated forward from here to the
				// local tick, so that it sits where the object is now rather than
				// where it was when the packet left.
				//
				// Applying the raw sample was the first attempt and it does not work.
				// At 60 u/s a four-tick-old sample is two units behind, which is twice
				// the size of the objects the workloads build, so the owned object
				// reaches the real contact point before the shadow appears to get
				// there and passes straight through.
				uint64_t sampleTick = 0;
				// Tick a state was last applied on. A shadow with nothing newer for a
				// while has left the band, been handed off or been destroyed, and is
				// retired rather than left behind as an invisible wall.
				uint64_t lastAppliedTick = 0;
			};
			std::map<int, HaloAuthoritativeState> mHaloState;

			// Halo updates waiting for their scheduled tick. Held by value: a
			// HaloObjectState is POD and small, unlike the handoff packet.
			struct ScheduledHaloUpdate {
				int objectID = -1;
				int archetypeID = 0;
				int ownerServerID = -1;
				uint64_t applyAtTick = 0;
				HaloAuthoritativeState state;
			};
			std::vector<ScheduledHaloUpdate> mScheduledHaloUpdates;
			void FlushScheduledHaloUpdates();

			// Releases scheduled by ScheduleOutgoingObject, keyed by object id so the
			// border check can test membership cheaply.
			struct ScheduledRelease {
				int newOwnerServerID = -1;
				uint64_t releaseAtTick = 0;
			};
			std::map<int, ScheduledRelease> mScheduledReleases;
			void FlushScheduledReleases();

			// Adopts any queued partition whose effective tick has arrived.
			void FlushPendingPartitions();
			std::vector<PendingPartition> mPendingPartitions;
			int mRepartitionsLate = 0;
			int mLoadReportIntervalTicks = 0;
			// Contacts since the last report. Deterministic, unlike a duration.
			long long mContactsSinceReport = 0;
			uint64_t mLastLoadReportTick = 0;
			int mRepartitionCount = 0;

			// Turns an existing halo shadow into an object this server owns, rather
			// than tearing the shadow down and building a replacement.
			//
			// Cheaper, but the reason is correctness as much as cost: rebuilding
			// discards the object's contact history, which UpdateCollisionList carries
			// for several frames, so an object mid-collision at the border would have
			// its contacts silently reset by the transfer.
			CSC8503::GameObject* PromoteHaloShadow(int networkID);

			// Builds a shadow. Deliberately NOT CreateObjectFromArchetype: that adds
			// the object to mCreatedObjectPool and to mNetworkObjects, which would
			// make a shadow look like one of this server's own objects to the
			// snapshot loop, the border check and the locality metric.
			CSC8503::GameObject* CreateHaloShadow(int archetypeID, int networkID,
				const HaloAuthoritativeState& state);

			// Copies each shadow's authoritative state back over whatever the previous
			// tick's contact resolution did to it.
			void ReimposeHaloState();

			// Tears down shadows that have had no update for HALO_STALE_TICKS.
			//
			// Not housekeeping: a shadow whose owner has stopped publishing it is an
			// obstacle sitting where nothing exists any more. Leaving them in place
			// pushed every object in a headon run onto one server.
			void RetireStaleHaloShadows();

			// Removes the shadow for an object this server has just taken ownership
			// of, so the same object is not in the broadphase twice.
			void RemoveHaloShadow(int networkID);

			// Deliberately SEPARATE from mHandoffLookaheadTicks, and much smaller.
			//
			// The two are not the same kind of delay. A handoff releases the object at
			// senderTick and the receiver picks it up at senderTick + lookahead; the
			// object is frozen in between, so a large value only widens a gap on a
			// rare event. A halo update is a continuously tracked position: applying
			// it `lookahead` ticks late means the shadow is that far behind reality
			// permanently, and an owned object would be colliding with where its
			// neighbour used to be. At the 300-tick handoff lookahead used for
			// reproducible runs that is 2.5 seconds of lag, which is worse than having
			// no shadow at all.
			//
			// So this is a small number - just enough to cover LAN delivery jitter -
			// and haloLate counts the updates that still miss their slot.
			int mHaloLookaheadTicks = 4;

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
			// The cache was rebuilt only when the map's SIZE changed, which is fine
			// while borders are fixed and silently wrong once they can move: a
			// repartition changes the values, not the count. Ownership then kept
			// answering from the old partition while the halo, which reads the map
			// directly, had already moved to the new one.
			mutable bool mRegionsDirty = true;
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
