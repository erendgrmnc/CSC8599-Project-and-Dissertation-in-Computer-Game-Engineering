#pragma once

#ifdef USEGL
#include <queue>
#include <mutex>
#include <functional>
#include <random>
#include "NetworkedGame.h"
#include <imgui/imgui.h>



namespace NCL::CSC8503 {
	struct DistributedClientGetGameInstanceDataPacket;
}

namespace NCL::CSC8503
{
	struct DistributedClientConnectToPhysicsServerPacket;
	struct SyncPlayerIdNameMapPacket;
}

namespace NCL::CSC8503
{
    struct ClientInitPacket;
}

namespace NCL::CSC8503
{
    struct SyncObjectStatePacket;
}

namespace NCL::CSC8503 {
    struct SyncInteractablePacket;
    struct ClientSyncItemSlotPacket;
}

namespace NCL{
    namespace CSC8503{
        struct DeltaPacket;
        class GameServer;
        class GameClient;
        class NetworkPlayer;

        struct FullPacket;
        struct ClientPlayerInputPacket;
        struct ClientSyncBuffPacket;
        struct AddPlayerScorePacket;
        struct ClientSyncLocalActiveSusCausePacket;
        struct ClientSyncLocalSusChangePacket;
        struct ClientSyncGlobalSusChangePacket;
        struct ClientSyncLocationActiveSusCausePacket;
        struct ClientSyncLocationSusChangePacket;
        struct AnnouncementSyncPacket;
        struct GuardSpotSoundPacket;

        class MultiplayerGameScene : public NetworkedGame{
        public:
            MultiplayerGameScene();
            ~MultiplayerGameScene();
            
            bool GetIsServer() const;
            bool PlayerWonGame() override;
            bool PlayerLostGame() override;
            const bool GetIsGameStarted() const;

            bool StartAsServer(const std::string& playerName);
            bool StartAsClient(char a, char b, char c, char d, const std::string& playerName);
            bool StartAsClient(char a, char b, char c, char d, const std::string& playerName, const std::string& gameInstanceID);
	
            void UpdateGame(float dt) override;

            void SetIsGameStarted(bool isGameStarted, int gameInstanceID, unsigned int seed = -1);
            void SetIsGameFinished(bool isGameFinished, int winningPlayerId);
            void StartLevel(const std::mt19937& levelSeed);

            void AddEventOnGameStarts(std::function<void()> event);

            void ReceivePacket(int type, GamePacket* payload, int source) override;
            void InitInGameMenuManager() override;

            void SendInteractablePacket(int networkObjectId, bool isOpen, int interactableItemType) const;
            void SendClientSyncItemSlotPacket(int playerNo, int invSlot, int inItem, int usageCount) const;
            void SendClientSyncBuffPacket(int playerNo, int buffType, bool toApply) const;
            void SendObjectStatePacket(int networkId, int state) const;
            void ClearNetworkGame();

            void SendClientSyncLocalActiveSusCausePacket(int playerNo, int activeSusCause, bool toApply) const;
            void SendClientSyncLocalSusChangePacket(int playerNo, int changedValue) const;
            void SendClientSyncGlobalSusChangePacket(int changedValue) const;
            void SendClientSyncLocationActiveSusCausePacket(int cantorPairedLocation, int activeSusCause, bool toApply) const;
            void SendClientSyncLocationSusChangePacket(int cantorPairedLocation, int changedValue) const;

            void SendAnnouncementSyncPacket(int annType, float time,int playerNo);

            void SendGuardSpotSoundPacket(int playerId) const;

            void SendPacketsThread();

            void SendGameClientConnectedToDistributedManagerPacket(int gameInstanceID);

            GameClient* GetClient() const;
            GameServer* GetServer() const;
            NetworkPlayer* GetLocalPlayer() const;

        protected:
            bool mIsGameStarted = false;
            bool mIsGameFinished = false;
            bool mIsServer = false;
            bool mShowDebugInfo = true;
            bool mObjLoggerStarted = false;

            float mObjLogTimer = 0.f;
            int mLogCtr = 0;

            int mWinningPlayerId;
            int mLocalPlayerId;
            int mGameInstanceID = -1;
            int mTotalPlayerCount = 1;
            int mObjectsPerPlayer = 1;

            int prevServerOfObj = -1; // TEST VAR

            ImFont* profilerFont = nullptr;

            void UpdateAsServer(float dt);
            void UpdateAsClient(float dt);

            void BroadcastSnapshot(bool deltaFrame);
            void UpdateMinimumState();
            int GetPlayerPeerID(int peerId = -2);

            void SendStartGameStatusPacket(const std::string& seed = "") const;
            void SendFinishGameStatusPacket();
            
            void InitWorld(const std::mt19937& levelSeed);

            void HandleClientPlayerInput(ClientPlayerInputPacket* playerMovementPacket, int playerPeerID);

            void SpawnPlayers();

            void LogObjectPositionToTxtFile(const Maths::Vector3& position, int timeAsSecond);

        	NetworkPlayer* AddPlayerObject(const Maths::Vector3& position, int playerNum);

            void HandleFullPacket(FullPacket* fullPacket);

            void HandleDeltaPacket(DeltaPacket* deltaPacket);

            void HandleClientPlayerInputPacket(ClientPlayerInputPacket* clientPlayerInputPacket, int playerPeerId);

            void HandleAddPlayerScorePacket(AddPlayerScorePacket* packet);

            void SyncPlayerList();

        	void SetItemsLeftToZero() override;

            void HandlePlayerEquippedItemChange(ClientSyncItemSlotPacket* packet) const;

            void HandleInteractablePacket(SyncInteractablePacket* packet) const;

        	void HandlePlayerBuffChange(ClientSyncBuffPacket* packet) const;

            void HandleObjectStatePacket(SyncObjectStatePacket* packet) const;

            void HandleLocalActiveSusCauseChange(ClientSyncLocalActiveSusCausePacket* packet) const;
            void HandleLocalSusChange(ClientSyncLocalSusChangePacket* packet) const;
            void HandleGlobalSusChange(ClientSyncGlobalSusChangePacket* packet) const;
            void HandleLocationActiveSusCauseChange(ClientSyncLocationActiveSusCausePacket* packet) const;
            void HandleLocationSusChange(ClientSyncLocationSusChangePacket* packet) const;

            void HandleAnnouncementSync(const AnnouncementSyncPacket* packet) const;

            void HandleGuardSpotSound(GuardSpotSoundPacket* packet) const;

            void AddToPlayerPeerNameMap(int playerId, const std::string& playerName);
            void HandleClientInitPacket(const ClientInitPacket* packet, int playerID);

            void WriteAndSendSyncPlayerIdNameMapPacket() const;
            void HandleSyncPlayerIdNameMapPacket(const SyncPlayerIdNameMapPacket* packet);

            //Distributed System functions
        	void HandleOnConnectToDistributedPhysicsServerPacketReceived(DistributedClientConnectToPhysicsServerPacket* packet);
            void HandleOnDistributedGameClientGameInstanceDataPacketReceived(DistributedClientGetGameInstanceDataPacket* packet);
            bool ConnectClientToDistributedGameServer(char a, char b, char c, char d, int port, int gameServerID, const std::string& playerName);
        	std::vector<char> IpToCharArray(const std::string& ipAddress);

        	void ShowPlayerList() const;

            void DrawCanvas() override;

            std::vector<std::function<void()>> mOnGameStarts;

            int mNetworkObjectCache = 10;

            int mServerSideLastFullID;

            std::queue<GamePacket*> mPacketToSendQueue;
            std::mutex mPacketToSendQueueMutex;

            std::map<int, std::string> mPlayerPeerNameMap;
            std::map<int, NCL::CSC8503::GameClient*> mDistributedPhysicsClients;
        private:
        };
    }
}
#endif