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

			void SendReliablePacket(GamePacket& payload) const;

			virtual bool UpdateClient();

			void WriteAndSendClientInputPacket(int playerID);

			void WriteAndSendClientUseItemPacket(int playerID, int objectID);

			void Disconnect();

			bool GetIsConnected() const;

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

			int mPeerId;
			int mClientSideLastFullID;

			std::string mPlayerName;
			
			_ENetPeer*	mNetPeer;
			float mTimerSinceLastPacket;
			PlayerInputs* mPlayerInputs;

			void SendClientInitPacket();

			std::vector<std::function<void()>> mOnClientConnectedToServer;
		};
	}
}

#endif