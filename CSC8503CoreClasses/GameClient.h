#include "Ray.h"
#ifdef USEGL
#pragma once
#include "NetworkBase.h"
#include <stdint.h>
#include <thread>
#include <atomic>

namespace NCL::CSC8503{
	struct PlayerInputs;
}

namespace NCL {
	namespace CSC8503 {

		struct PlayerInputs {
			bool isSprinting = false;
			bool isCrouching = false;
			bool isUp = false;
			bool isDown = false;
			bool isEquippedItemUsed = false;
			bool isInteractButtonPressed = false;
			bool isHoldingInteractButton = false;

			int leftHandItemId = 0;
			int rightHandItemId = 0;

			bool movementButtons[4] = { false };

			float cameraYaw;

			Maths::Vector3 fwdAxis;
			Maths::Vector3 rightAxis;
			Maths::Ray rayFromPlayer;
		};

		class GameObject;
		class GameClient : public NetworkBase {
		public:
			GameClient();
			~GameClient();

			int GetPeerID() const;

			const int GetClientLastFullID() const;
			void SetClientLastFullID(const int clientLastFullID);

			bool Connect(uint8_t a, uint8_t b, uint8_t c, uint8_t d, int portNum, const std::string& playerName);

			void SendPacket(GamePacket&  payload);

			// Returns whether ENet accepted the packet. A caller that releases state on
			// the strength of a send - handoff does - must check it.
			bool SendReliablePacket(GamePacket& payload) const;

			virtual bool UpdateClient();

			void WriteAndSendClientInputPacket(int playerID);

			void WriteAndSendClientUseItemPacket(int playerID, int objectID);

			void Disconnect();

			bool GetIsConnected() const;
			void RegisterOnDisconnectedEvent(const std::function<void()>& callback) {
				mOnClientDisconnectedFromServer.push_back(callback);
			}

			// True only once ENet has REPORTED the peer going away.
			//
			// Deliberately not the inverse of GetIsConnected(). Connect() returns as
			// soon as enet_host_connect has allocated a peer - it does not wait for
			// the handshake - so mIsConnected is false for a window after a link is
			// created and perfectly healthy. Polling that to decide a link is dead
			// tears down every link during bootstrap.
			bool HasLostLink() const { return mLinkLost; }

			void WriteAndSendAnnouncementSyncPacket(int annType, float time, int playerNo);

			void WriteAndSendInteractablePacket(int networkObjectId, bool isOpen, int interactableItemType);

			void WriteAndSendInventoryPacket(int playerNo, int invSlot, int inItem, int usageCount);

			void WriteAndSendSyncLocationSusChangePacket(int cantorPairedLocation, int changedValue);

			void SetPlayerInputs(PlayerInputs& playerInputs);
			void SetPlayerInputs(bool movementButtons[4]);


			void AddOnClientConnected(const std::function<void()>& callback);

			std::string GetIPAddress();
		protected:
			bool mIsConnected;
			// See HasLostLink.
			bool mLinkLost = false;

			int mPeerId;
			int mClientSideLastFullID;

			std::string mPlayerName;
			
			_ENetPeer*	mNetPeer;
			float mTimerSinceLastPacket;
			PlayerInputs* mPlayerInputs;

			void SendClientInitPacket();

			std::vector<std::function<void()>> mOnClientConnectedToServer;
			// Fired when a live link drops, as opposed to being closed on purpose.
			// The owner needs this to rebuild the link: nothing else can observe an
			// ENet peer going away.
			std::vector<std::function<void()>> mOnClientDisconnectedFromServer;
		};
	}
}

#endif