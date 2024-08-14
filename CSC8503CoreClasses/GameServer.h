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

			std::string GetIpAddress() const;

			virtual void UpdateServer();
			void SetMaxClients(int maxClients);

		protected:
			int			mPort;
			int			mClientMax;
			int			mClientCount;
			int*        mPeers;
			GameWorld*	mGameWorld;

			int mIncomingDataRate;
			int mOutgoingDataRate;

			char ipString[16];
		};
	}
}
#endif