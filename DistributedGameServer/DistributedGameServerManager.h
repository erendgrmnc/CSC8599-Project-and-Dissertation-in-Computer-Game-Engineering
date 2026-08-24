#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <queue>
#include <vector>

#include "DistributedPhysicsServerClient.h"
#include "NetworkBase.h"
#include "NetworkObject.h"
#include "DistributedSystemCommonFiles/SequenceWindow.h"
#include "DistributedSystemCommonFiles/RegionOwnership.h"

namespace NCL::CSC8503 {
	struct DistributedManagerAllGameServersAreConnectedPacket;
	struct GameStartStatePacket;
}

namespace NCL::Networking {
	class DistributedPacketSenderServer;
}

namespace NCL::CSC8503 {
	class NetworkObject;
}

namespace NCL::DistributedGameServer {
	struct PhysicsServerBorderData;
	class ServerWorldManager;
}


namespace NCL {
	namespace DistributedGameServer {
		struct GameServerConnection {
			int serverID;
			GameClient* client = nullptr;

			GameServerConnection(int serverID, GameClient* client);
		};

		class DistributedGameServerManager : public PacketReceiver {
		public:
			// Halo updates reliably rather than unreliably. See PublishHaloBand:
			// needed for a reproducible run, not for a deployment.
			void SetHaloReliable(bool reliable) { mHaloReliable = reliable; }

			// --- injected link delay (Phase C) -------------------------------------
			//
			// Holds outbound SERVER-TO-SERVER packets for a configured one-way delay
			// before handing them to ENet, so the halo soundness bound can be swept
			// over a latency axis instead of only at the zero-latency special case
			// that every measurement before this ran at.
			//
			// Applied to the peer path only. The client path is deliberately NOT
			// delayed: the halo bound is a statement about server-to-server lag, and
			// delaying snapshots would move E3 and E8 without moving E5, which is the
			// experiment this exists for.
			//
			// jitterMs is a WORST-CASE additional delay, drawn deterministically from
			// the world seed so a run stays reproducible. Release times are forced
			// monotonic per target, so jitter never reorders packets on a link - real
			// networks do reorder, but a bound test wants the delay axis varied and
			// nothing else.
			void SetLinkDelay(float latencyMs, float jitterMs, unsigned int seed);

			// Sends everything whose release time has passed. Called once per tick.
			void DrainDelayedPeerPackets();

			int GetDelayedPeerPacketCount() const {
				return static_cast<int>(mDelayedPeerPackets.size());
			}

			// Per-host ENet totals, split by what each host family carries. The
			// sender server talks to CLIENTS (snapshots, spawns, acks); the peer
			// links talk to other SERVERS (halo, handoffs). E8 needs exactly that
			// split, and here it falls out of the topology instead of being
			// attributed after the fact.
			struct NetworkByteTotals {
				// Mirrors the wrap note on NetworkBase::GetTotalSentData/GetTotalSentPackets
				// (NetworkBase.h:188-190): each host's own counter is an enet_uint32 that
				// wraps after ~4 GB (~15 minutes at E8's measured rates). The peer fields
				// here are worse, not just as exposed: GetNetworkByteTotals() SUMS several
				// peer links' host counters into one field of this struct, so the sum can
				// wrap before any individual host's own counter does. Safe at the durations
				// this project measures; a longer run needs 64-bit accumulation during the
				// run rather than a read once at exit.
				unsigned int clientBytes = 0;
				unsigned int clientPackets = 0;
				unsigned int peerBytes = 0;
				unsigned int peerPackets = 0;
			};
			NetworkByteTotals GetNetworkByteTotals() const;

			DistributedGameServerManager(int serverID, int gameInstanceID,  const std::string& serverBordersStr);
			~DistributedGameServerManager();

			NCL::Networking::DistributedPhysicsServerClient* GetDistributedPhysicsServer() const;
			NCL::DistributedGameServer::ServerWorldManager* GetServerWorldManager() const;

			bool StartDistributedGameServer(char a, char b, char c, char d, int port);
			bool StartDistributedPacketSenderServer();
			bool GetGameStarted() const;

			void UpdateGameServerManager(float dt);
			void RegisterGameServerPackets();
			void RegisterPacketSenderServerPackets();
			void UpdateMinimumState();
			void HandleClientPlayerInputPacket(ClientPlayerInputPacket* packet, int playerPeerID);
			void HandleClientSnapshotAckPacket(CSC8503::DistributedClientSnapshotAckPacket* packet, int source);
			void ReceivePacket(int type, GamePacket* payload, int source) override;
			void BroadcastSnapshot(bool deltaFrame);
			void SendPacketsThread();
			void HandleGameStarted(CSC8503::GameStartStatePacket* gameStartPacket);
			void StartGame();
			void SendAllClientsAreConnectedToPacketSenderServerPacket() const;
			void SendPacketSenderServerStartedPacket(int port) const;
		
			void HandleTransitionHandshakePacketReceived(StartSimulatingObjectReceivedPacket* packet);

		protected:
			bool mIsServerConnectedToManager = false;
			bool mIsGameStarted = false;
			bool mIsPlayerObjectsCreated = false;

			int mGameServerID;
			int mGameInstanceID;
			int mServerSideLastFullID;
			int mPacketsToSnapshot;

			int mPacketSenderServerPort;
			int mMaxGameClientsToConnectPacketSender;

			float mTimeToNextPacket;
			float mDebugTimer = 5.f;

			std::queue<GamePacket*> mPacketToSendQueue;

			std::mutex mPacketToSendQueueMutex;

			std::vector<CSC8503::NetworkObject*>* mNetworkObjects;

			std::map<int, int> mStateIDs;

			// (playerID, sequence) is the dedupe key for client commands. Per-player,
			// because sequences are client-scoped and two clients will collide.
			std::map<int, NCL::SequenceWindow> mClientCommandWindows;

			// (originServerID, originSequence) for relays. A relayed area effect can
			// reach one server through two different neighbours; without this an
			// object inside both radii is pushed twice.
			std::map<int, NCL::SequenceWindow> mRelayWindows;
			int mRelaySequenceCounter = 0;

			// Hop count of the relay currently being handled, so a relay emitted while
			// handling a relay is stamped 1 and the loop guard actually fires. The
			// packet constructor cannot know this - it always stamps 0 - so without
			// this the guard could never trigger.
			int mCurrentRelayHop = 0;

			int mCommandsApplied = 0;
			int mCommandsRelayed = 0;
			int mCommandsDuplicate = 0;
			int mCommandsRejected = 0;
			// Area-effect fan-out hops sent from this server. An area command applies
			// once per overlapped region by design, so I4 only balances once these are
			// subtracted from the applied total.
			int mCommandsFannedOut = 0;
			int mObjectsSpawned = 0;
			int mObjectsDestroyed = 0;
			int mManifestEntriesSent = 0;
			// Halo traffic, packets and objects, both directions. Objects rather than
			// just packets because the batch size is the thing that makes this
			// affordable, and a packet count alone would hide it.
			// Set from --halo-reliable. See PublishHaloBand.
			bool mHaloReliable = false;
			// The tick the halo band was last published on, so it is published once per
			// tick rather than once per loop iteration. See PublishHaloBand. The
			// sentinel is a tick value the counter cannot reach, so the first publish
			// on tick 0 is not mistaken for a repeat.
			uint64_t mLastHaloPublishTick = UINT64_MAX;
			// The instance's server registry, assembled from pages. Keyed by server id,
			// so a page delivered twice overwrites rather than appending and the
			// completeness test cannot be satisfied by duplicates.
			//
			// This replaces the five fixed 20-entry arrays that used to ride inside
			// StartDistributedGameServerPacket, which capped an instance at 20 servers
			// and made that one message about 6 KB.
			std::map<int, CSC8503::ServerRegistryEntry> mServerRegistry;
			int mRegistryTotalServerCount = 0;
			// From the start packet, needed to size the packet sender's peer bound.
			int mExpectedClientCount = 0;

			// Partitions still arriving, keyed by effective tick. A partition is only
			// scheduled once every one of its regions has been received; adopting a
			// partial one would leave this server disagreeing with its peers about
			// where the borders are.
			std::map<long long, std::map<int, NCL::Interaction::RegionBounds>>
				mAssemblingPartitions;

			// What each peer has asked to be told about, keyed by the SendPacketToPeer
			// peer number (source + 1).
			//
			// A peer with no entry here gets every snapshot, which is what the system
			// did before interest existed and what a peer that is another SERVER
			// should keep getting - servers do not consume snapshots, but excluding
			// them would mean deciding which peers are servers, and a peer that never
			// declares interest is already cheap to serve correctly.
			struct PeerInterest {
				Maths::Vector3 centre;
				float radius = 0.0f;   // <= 0 means "everything"
			};
			std::map<int, PeerInterest> mPeerInterest;
			// Re-declares "send me no snapshots" to peer servers. See the send site.
			float mPeerInterestDeclareTimer = 0.0f;

			// Object-snapshots actually put on the wire, and the ones interest
			// suppressed. The ratio between them is the whole point of the increment,
			// so it is measured rather than argued.
			long long mSnapshotsSent = 0;
			long long mSnapshotsSuppressed = 0;

			int mHaloUpdatesSent = 0;
			int mHaloObjectsSent = 0;
			int mHaloUpdatesReceived = 0;
			int mHaloObjectsReceived = 0;

			std::map<const int, PhysicsServerBorderData*> mPhysicsServerBorderMap;

			NCL::Networking::DistributedPhysicsServerClient* mThisDistributedPhysicsServer = nullptr;
			NCL::Networking::DistributedPacketSenderServer* mDistributedPacketSenderServer = nullptr;
			std::vector<GameServerConnection*> mDistributedPhysicsClients;

			NCL::DistributedGameServer::ServerWorldManager* mServerWorldManager;

			std::vector<char> IpToCharArray(const std::string& ipAddress);

			PhysicsServerBorderData* CreatePhysicsServerBorders(const std::string& borderString);

			void HandleStartGameServerPacketReceived(StartDistributedGameServerPacket* packet);
			void HandleObjectTransitions() const;

			// Returns false if the packet could not be handed to a link for the new
			// owner. The caller MUST NOT release the object in that case - a directed
			// send has no broadcast to fall back on, so releasing after a failed send
			// destroys the object outright.
			bool SendFinishTransactionPacket(NetworkObject& obj, StartSimulatingObjectPacket& outSent) const;

			// Directed reliable send to one peer server over the existing mesh.
			// Handoffs and relays both need this; it is deliberately one function so
			// the serverID-vs-array-index hazard is solved in a single place.
			bool SendPacketToServer(int targetServerID, GamePacket& packet) const;
			// For state superseded every tick. See the definition.
			bool SendUnreliablePacketToServer(int targetServerID, GamePacket& packet) const;


			// Whether a peer link to that server exists AT ALL, independently of
			// whether any particular send succeeded. Custody needs the distinction:
			// a send can fail because ENet's outgoing reliable queue is full (overload)
			// or because the peer is gone, and only the second may reclaim an object.
			bool HasPeerLink(int targetServerID) const;

			// Per-target count of "no peer link" failures, so the message can be
			// logged on a 1, 10, 100, ... schedule instead of once per object per
			// tick. A lost link fails EVERY pending handoff EVERY tick, and the
			// unthrottled form wrote 233,654 lines down the midware pipe on a single
			// 60 s run - enough I/O to dominate the run it was reporting on, and to
			// perturb any measurement taken while it was happening. Mutable because
			// the send paths that hit it are const.
			mutable std::map<int, long long> mMissingLinkFailures;
			// True when this failure should be printed. Always true the first time
			// for a given target.
			bool ShouldLogMissingLink(int targetServerID) const;

			// Fault injection (see ServerWorldManager::SetHandoffDelayTicks). The
			// object is released locally at the normal moment; only the transfer
			// packet is held back, which is precisely the window race W3 needs.
			struct DelayedHandoff {
				CSC8503::StartSimulatingObjectPacket packet;
				int ticksRemaining = 0;
			};
			mutable std::vector<DelayedHandoff> mDelayedHandoffs;
			void FlushDelayedHandoffs();
			void SendTransactionHandshakePacket(int senderServerID, int networkID) const;

			// --- interaction command channel ---
			void HandleClientCommandPacket(CSC8503::DistributedClientCommandPacket* packet);
			void HandleServerCommandRelayPacket(CSC8503::DistributedServerCommandRelayPacket* packet);
			void DispatchCommand(NCL::Interaction::CommandType type,
				const NCL::Interaction::CommandArgs& args, int playerID, int clientSequence);
			void DrainPendingRelays(int playerID, int clientSequence);
			void DrainPendingSpawns();
			void HandleObjectSpawnedPacket(CSC8503::DistributedObjectSpawnedPacket* packet);
			void HandleHaloUpdatePacket(CSC8503::HaloUpdatePacket* packet);
			void HandleRepartitionPacket(CSC8503::DistributedRepartitionPacket* packet);
			void HandleServerRegistryPacket(CSC8503::DistributedServerRegistryPacket* packet);
			void HandleClientInterestPacket(CSC8503::DistributedClientInterestPacket* packet, int source);
			// Builds borders and peer links from a COMPLETE registry. Idempotent: the
			// registry is rebroadcast as servers register, so this runs repeatedly.
			void ApplyServerRegistry();
			// Publishes this server's border objects to the neighbours whose regions
			// they are close to. Called once per tick while the game is running.
			void PublishHaloBand();
			void DrainPendingDespawns();
			void HandleObjectDespawnedPacket(CSC8503::DistributedObjectDespawnedPacket* packet);
			void SendManifestToPeer(int peerNumber);
			void SendCommandAck(int sequence, int playerID, int targetObjectID,
				NCL::Interaction::CommandResult result, int correctedServerID);
			GameServerConnection* ConnectServerToAnotherGameServer(char a, char b, char c, char d, int port, int gameServerID);

			// A peer whose sender server was not listening yet when we first tried.
			// Servers come up in an arbitrary order, so a failed connect is routine -
			// what is not acceptable is treating it as success, which left the target
			// waiting forever for a peer that never arrived.
			struct PendingPeer {
				std::vector<char> ip;
				int port = 0;
				int serverID = -1;
			};
			std::vector<PendingPeer> mPendingPeers;
			float mPeerRetryTimer = 0.0f;
			void RetryPendingPeers(float dt);
			// Requeues links ENet has dropped so RetryPendingPeers rebuilds them.
			void ReclaimDroppedPeers();

			// Copies the packet and queues it. Returns false only when there is no peer
			// link, matching SendPacketToServer's contract - custody reclaims solely on
			// a MISSING link, so a queued packet must report success or a delayed
			// handoff would be reclaimed while still in flight.
			bool QueueDelayedPeerPacket(int targetServerID, GamePacket& packet,
				bool reliable) const;

			struct DelayedPeerPacket {
				int targetServerID = -1;
				bool reliable = true;
				long long releaseMicros = 0;
				std::vector<char> bytes;
			};

			// mutable: the two send functions are const, and threading the delay
			// through them by dropping const would touch every call site for no gain.
			mutable std::vector<DelayedPeerPacket> mDelayedPeerPackets;
			// Last release time issued per target, so release times stay monotonic and
			// jitter cannot reorder a link.
			mutable std::map<int, long long> mLastReleaseMicros;
			mutable unsigned int mJitterCounter = 0;
			float mLinkLatencyMs = 0.0f;
			float mLinkJitterMs = 0.0f;
			unsigned int mLinkDelaySeed = 1u;
			// Links ENet has dropped, kept alive rather than destroyed. See
			// ReclaimDroppedPeers for why they are not deleted during a run.
			std::vector<GameServerConnection*> mRetiredPeerLinks;
		};
	}
}

