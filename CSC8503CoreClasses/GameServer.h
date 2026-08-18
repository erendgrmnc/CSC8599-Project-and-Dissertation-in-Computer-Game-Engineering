#ifdef USEGL
#pragma once
#include "NetworkBase.h"

namespace NCL {
	namespace CSC8503 {
		class GameWorld;
		class GameServer : public NetworkBase {
		public:
			GameServer(int onPort, int maxClients, bool isStartingServer = true);
			~GameServer();

			bool Initialise();
			void Shutdown();

			void SetGameWorld(GameWorld &g);
			virtual void AddPeer(int peerNumber);

			bool SendGlobalPacket(int msgID);
			bool SendGlobalReliablePacket(GamePacket& packet);
			bool SendGlobalPacket(GamePacket& packet);
			bool SendVariableUpdatePacket(VariablePacket& packet);
			bool GetPeer(int peerNumber, int& peerId) const;

			// Directed send to one peer. Needed for a late-join manifest: broadcasting
			// it would make every already-connected client re-receive the whole world
			// each time anyone joins. mPeers holds peer NUMBERS, not handles, so the
			// ENetPeer* has to be retained separately - which is why this could not
			// simply be written in terms of the existing peer table.
			bool SendPacketToPeer(int peerNumber, GamePacket& packet);

			// Peer numbers currently connected, in the numbering SendPacketToPeer uses.
			// Needed by anything that sends a DIFFERENT packet to different peers -
			// interest-filtered snapshots being the reason it exists.
			std::vector<int> GetConnectedPeers() const;

			std::string GetIpAddress() const;

			virtual void UpdateServer();
			virtual void SetMaxClients(int maxClients);

		protected:
			int			mPort;
			int			mClientMax;
			int			mClientCount;
			int*        mPeers;
			// Parallel to mPeers: the ENet handle for each peer number, so a directed
			// send can find its destination. Kept as a map rather than an array so it
			// cannot fall out of step with mClientMax.
			std::map<int, _ENetPeer*> mPeerHandles;

			// What enet_host_create was actually given. Fixed for the life of the
			// host, so SetMaxClients can warn rather than silently promising room the
			// host does not have.
			int mHostPeerCapacity = 0;
			GameWorld*	mGameWorld;

			int mIncomingDataRate;
			int mOutgoingDataRate;

			char ipAddress[16];
		};
	}
}
#endif