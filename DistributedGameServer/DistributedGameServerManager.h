#pragma once
#include <mutex>
#include <queue>

#include "DistributedPhysicsServerClient.h"
#include "NetworkBase.h"
#include "NetworkObject.h"
#include "DistributedSystemCommonFiles/SequenceWindow.h"

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

			std::map<const int, PhysicsServerBorderData*> mPhysicsServerBorderMap;

			NCL::Networking::DistributedPhysicsServerClient* mThisDistributedPhysicsServer = nullptr;
			NCL::Networking::DistributedPacketSenderServer* mDistributedPacketSenderServer = nullptr;
			std::vector<GameServerConnection*> mDistributedPhysicsClients;

			NCL::DistributedGameServer::ServerWorldManager* mServerWorldManager;

			std::vector<char> IpToCharArray(const std::string& ipAddress);

			PhysicsServerBorderData* CreatePhysicsServerBorders(const std::string& borderString);

			void HandleStartGameServerPacketReceived(StartDistributedGameServerPacket* packet);
			void HandleObjectTransitions() const;
			void SendFinishTransactionPacket(NetworkObject& obj) const;

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
		};
	}
}

