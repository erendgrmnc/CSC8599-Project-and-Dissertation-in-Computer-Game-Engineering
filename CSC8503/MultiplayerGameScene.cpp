﻿#include <fstream>
#include <imgui/imgui.h>

#include "PhysicsObject.h"
#include "Profiler.h"
#ifdef USEGL
#include "MultiplayerGameScene.h"

#include <iostream>
#include <string>

#include "GameServer.h"
#include "GameClient.h"
#include "Helipad.h"
#include "Interactable.h"
#include "InteractableDoor.h"
#include "MultiplayerStates.h"
#include "NetworkObject.h"
#include "NetworkPlayer.h"
#include "PushdownMachine.h"
#include "RenderObject.h"
#include "LevelManager.h"
#include "../CSC8503/InventoryBuffSystem/InventoryBuffSystem.h"
#include "../CSC8503/InventoryBuffSystem/FlagGameObject.h"
#include "../CSC8503/SuspicionSystem/SuspicionSystem.h"
#include "Vent.h"
#include "Debug.h"

namespace {
	constexpr int MAX_PLAYER = 4;
	constexpr int SERVER_PLAYER_PEER = 0;

	constexpr const char* PLAYER_PREFIX = "Player";

	//PLAYER MENU
	constexpr Vector4 LOCAL_PLAYER_COLOUR(0, 0, 1, 1);
	constexpr Vector4 DEFAULT_PLAYER_COLOUR(1, 1, 1, 1);

	constexpr float VERTICAL_MARGIN_BETWEEN_PLAYER_NAMES = 5.f;

}

MultiplayerGameScene::MultiplayerGameScene() : mPlayerPeerNameMap() {

	mThisServer = nullptr;
	mThisClient = nullptr;

	mServerSideLastFullID = 0;
	mGameState = GameSceneState::MainMenuState;

	NetworkBase::Initialise();
	mTimeToNextPacket = 0.0f;
	mPacketsToSnapshot = 0;
	mWinningPlayerId = -1;
	mLocalPlayerId = -1;

	bool isEmpty = mPacketToSendQueue.empty();
	mTimeToNextPacket = 0.0f;
	mPacketsToSnapshot = -1;
	InitInGameMenuManager();

	for (int i = 0; i < MAX_PLAYER; i++) {
		mPlayerList.push_back(-1);
	}
	profilerFont = ImGui::GetIO().Fonts->AddFontFromFileTTF("fonts/BebasNeue-Regular.ttf", 13.f, NULL, ImGui::GetIO().Fonts->GetGlyphRangesDefault());
}

MultiplayerGameScene::~MultiplayerGameScene() {
}

bool MultiplayerGameScene::GetIsServer() const {
	return mIsServer;
}

bool MultiplayerGameScene::PlayerWonGame() {
	if (mIsGameFinished && mWinningPlayerId == mLocalPlayerId) {
		return true;
	}

	//TODO(erendgrmnc): lots of func calls, optimize it(ex: cache variables).

	if (auto* helipad = mLevelManager->GetHelipad()) {
		bool isAnyPlayerOnHelipad = false;
		std::tuple<bool, int> helipadCollisionResult = helipad->GetCollidingWithPlayer();
		isAnyPlayerOnHelipad = std::get<0>(helipadCollisionResult);
		if (std::get<0>(helipadCollisionResult)) {
			int playerIDOnHelipad = std::get<1>(helipadCollisionResult);
			if (mLevelManager->GetInventoryBuffSystem()->GetPlayerInventoryPtr()->ItemInPlayerInventory(PlayerInventory::flag, playerIDOnHelipad)) {
				if (mIsServer) {
					SetIsGameFinished(true, playerIDOnHelipad);
				}
				if (mLocalPlayerId == playerIDOnHelipad) {
					return true;
				}
			}
		}
	}

	return false;
}

bool MultiplayerGameScene::PlayerLostGame() {
	if (mIsGameFinished && mWinningPlayerId != mLocalPlayerId) {
		return true;
	}
	return false;
}

const bool MultiplayerGameScene::GetIsGameStarted() const {
	return mIsGameStarted;
}

bool MultiplayerGameScene::StartAsServer(const std::string& playerName) {
	mThisServer = new GameServer(NetworkBase::GetDefaultPort(), MAX_PLAYER);
	if (mThisServer) {

		mIsServer = true;

		mThisServer->RegisterPacketHandler(Received_State, this);
		mThisServer->RegisterPacketHandler(String_Message, this);
		mThisServer->RegisterPacketHandler(BasicNetworkMessages::ClientPlayerInputState, this);
		mThisServer->RegisterPacketHandler(BasicNetworkMessages::ClientInit, this);
		mThisServer->RegisterPacketHandler(BasicNetworkMessages::SyncAnnouncements, this);
		mThisServer->RegisterPacketHandler(BasicNetworkMessages::SyncInteractable, this);
		mThisServer->RegisterPacketHandler(BasicNetworkMessages::ClientSyncItemSlot, this);
		mThisServer->RegisterPacketHandler(BasicNetworkMessages::ClientSyncLocationSusChange, this);
		mThisServer->RegisterPacketHandler(BasicNetworkMessages::GuardSpotSound, this);

		AddToPlayerPeerNameMap(SERVER_PLAYER_PEER, playerName);

		std::thread senderThread(&MultiplayerGameScene::SendPacketsThread, this);
		mLevelManager->SetIsServer(true);
		senderThread.detach();
	}
	return mThisServer;
}

bool MultiplayerGameScene::StartAsClient(char a, char b, char c, char d, const std::string& playerName) {
	mThisClient = new GameClient();
	const bool isConnected = mThisClient->Connect(a, b, c, d, NetworkBase::GetDefaultPort(), playerName);

	if (isConnected) {
		mIsServer = false;
		mThisClient->RegisterPacketHandler(Delta_State, this);
		mThisClient->RegisterPacketHandler(Full_State, this);
		mThisClient->RegisterPacketHandler(Player_Connected, this);
		mThisClient->RegisterPacketHandler(Player_Disconnected, this);
		mThisClient->RegisterPacketHandler(String_Message, this);
		mThisClient->RegisterPacketHandler(GameStartState, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::SyncPlayers, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::GameEndState, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::ClientSyncItemSlotUsage, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::ClientSyncItemSlot, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::ClientSyncBuffs, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::ClientSyncLocalActiveCause, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::ClientSyncLocalSusChange, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::ClientSyncGlobalSusChange, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::ClientSyncLocationActiveCause, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::ClientSyncLocationSusChange, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::SyncInteractable, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::ClientSyncBuffs, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::SyncObjectState, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::SyncPlayerIdNameMap, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::SyncAnnouncements, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::GuardSpotSound, this);
		mThisClient->RegisterPacketHandler(DistributedClientConnectToPhysicsServer, this);
		mThisClient->RegisterPacketHandler(BasicNetworkMessages::DistributedClientGetGameInstanceData, this);

		//FOR DISTRIBUTED SYSTEM TEST
		//AddToPlayerPeerNameMap(SERVER_PLAYER_PEER, playerName);
	}

	mLevelManager->SetIsServer(false);



	return isConnected;

}

bool MultiplayerGameScene::StartAsClient(char a, char b, char c, char d, const std::string& playerName,
	const std::string& gameInstanceID) {

	bool isConnected = StartAsClient(a, b, c, d, playerName);

	if (isConnected) {
		std::function<void()> callback = [this, gameInstanceID] {
			int gameID = stoi(gameInstanceID);
			SendGameClientConnectedToDistributedManagerPacket(gameID);

			};
		mThisClient->AddOnClientConnected(callback);

	}

	return isConnected;
}

void MultiplayerGameScene::UpdateGame(float dt) {

	std::chrono::steady_clock::time_point start;
	std::chrono::steady_clock::time_point end;
	std::chrono::duration<double, std::milli> timeTaken;

	mTimeToNextPacket -= dt;
	if (mTimeToNextPacket < 0) {
		if (mThisServer) {
			UpdateAsServer(dt);
		}
		else if (mThisClient) {
			start = std::chrono::high_resolution_clock::now();
			UpdateAsClient(dt);
			end = std::chrono::high_resolution_clock::now();
			timeTaken = end - start;
			Profiler::SetNetworkTime(timeTaken.count());
		}
		mTimeToNextPacket += 1.0f / 60.0f; //20hz server/client update

		if (mThisServer && !mIsGameFinished) {
			SyncPlayerList();
		}
	}

	if (mPushdownMachine != nullptr) {
		mPushdownMachine->Update(dt);
	}

	if (mIsGameStarted && !mIsGameFinished) {
		ShowPlayerList();

		//TODO(erendgrmnc): rewrite this logic after end-game conditions are decided.

		mLevelManager->GetGameWorld()->GetMainCamera().UpdateCamera(dt);

		if (mThisServer) {
			Debug::Print("SERVER", Vector2(5, 10), Debug::MAGENTA);
		}
		else {
			if (Window::GetKeyboard()->KeyPressed(KeyCodes::P)) {
				mShowDebugInfo = !mShowDebugInfo;
			}

			if (mShowDebugInfo) {
				Debug::Print("CLIENT - Player ID: " + std::to_string(mLocalPlayerId), Vector2(5, 10), Debug::MAGENTA);

				int objectPerClient = 1;

				int testPeerId = mLocalPlayerId < 0 ? 0 : mLocalPlayerId * mObjectsPerPlayer;

				auto posVec = mNetworkObjects[testPeerId]->GetGameObject().GetTransform().GetPosition();
				auto predictedPosVec = mNetworkObjects[testPeerId]->GetGameObject().GetTransform().GetPredictedPosition();

				int serverID = mNetworkObjects[testPeerId]->GetGameObject().GetServerID();
				if (prevServerOfObj == -1) {
					prevServerOfObj = serverID;
				}

				std::string serverStr = "Object Server: " + std::to_string(serverID);
				std::string str = "Object Position: " + std::to_string(posVec.x) + ", " + std::to_string(posVec.y) + ", " + std::to_string(posVec.z);
				std::string predictedPosStr = "Predicted Object Position: " + std::to_string(predictedPosVec.x) + ", " + std::to_string(predictedPosVec.y) + ", " + std::to_string(predictedPosVec.z);

				Debug::Print(serverStr, Vector2(5, 15), Debug::GREEN);
				Debug::Print(str, Vector2(5, 25), Debug::RED);
				Debug::Print(predictedPosStr, Vector2(5, 35), Debug::BLUE);

				if (mObjLoggerStarted) {
					mObjLogTimer += dt;
					if (mObjLogTimer >= 1.f) {
						mLogCtr++;
						LogObjectPositionToTxtFile(posVec, mLogCtr);
						mObjLogTimer = 0;
					}
				}
			}
		}

		mLevelManager->Update(dt, mGameState == InitialisingLevelState, false);


		//TODO(erendgrmnc): create another func
		bool inputs[4] = { false,false,false,false };

		if (Window::GetKeyboard()->KeyDown(KeyCodes::J)) {
			inputs[0] = true;
		}

		if (Window::GetKeyboard()->KeyDown(KeyCodes::L)) {
			inputs[1] = true;
			if (mObjLoggerStarted == false) {
				mObjLoggerStarted = true;
			}
		}

		if (Window::GetKeyboard()->KeyDown(KeyCodes::I)) {
			inputs[2] = true;
		}

		if (Window::GetKeyboard()->KeyDown(KeyCodes::K)) {
			inputs[3] = true;
		}

		for (auto& client : mDistributedPhysicsClients) {
			client.second->SetPlayerInputs(inputs);
			client.second->WriteAndSendClientInputPacket(mLocalPlayerId);
		}
	}
	else {
		mLevelManager->GetRenderer()->Render();
	}

	if (mThisServer) {
		mThisServer->UpdateServer();
	}
	if (mThisClient) {
		mThisClient->UpdateClient();
	}
}

void MultiplayerGameScene::SetIsGameStarted(bool isGameStarted, int gameInstanceID, unsigned int seed) {
	if (mIsGameStarted == isGameStarted) {
		return;
	}

	this->mIsGameStarted = isGameStarted;

	int seedToUse = seed;
	if (isGameStarted) {
		mGameState = GameSceneState::InitialisingLevelState;
		if (mThisServer) {
			std::random_device rd;
			const unsigned int serverCreatedSeed = rd();

			const std::string seedString = std::to_string(serverCreatedSeed);

			SendStartGameStatusPacket(seedString);
			seedToUse = serverCreatedSeed;
		}

		std::mt19937 g(seedToUse);
		StartLevel(g);
	}
	else {
		if (mThisServer) {
			SendStartGameStatusPacket();
		}
	}
}

void MultiplayerGameScene::SetIsGameFinished(bool isGameFinished, int winningPlayerId) {
	mIsGameFinished = isGameFinished;
	mWinningPlayerId = winningPlayerId;
	if (mThisServer) {
		SendFinishGameStatusPacket();
	}
}

void MultiplayerGameScene::StartLevel(const std::mt19937& levelSeed) {
	InitWorld(levelSeed);
	Debug::Print("Game Started", Vector2(10, 5));

	for (auto& event : mOnGameStarts) {
		event();
	}
}

void MultiplayerGameScene::AddEventOnGameStarts(std::function<void()> event) {
	mOnGameStarts.push_back(event);
}

void MultiplayerGameScene::ReceivePacket(int type, GamePacket* payload, int source) {

	switch (type) {
	case BasicNetworkMessages::GameStartState: {
		GameStartStatePacket* packet = (GameStartStatePacket*)payload;
		unsigned int seed = 0;

		seed = 0;//std::stoul(packet->levelSeed);
		SetIsGameStarted(packet->isGameStarted, seed);
		break;
	}
	case BasicNetworkMessages::Full_State: {
		FullPacket* packet = (FullPacket*)payload;
		if (packet->objectID == 10) {
			int a = 0;
		}
		HandleFullPacket(packet);
		break;
	}
	case BasicNetworkMessages::Delta_State: {
		DeltaPacket* deltaPacket = (DeltaPacket*)payload;
		HandleDeltaPacket(deltaPacket);
		break;
	}
	case BasicNetworkMessages::GameEndState: {
		GameEndStatePacket* packet = (GameEndStatePacket*)payload;
		SetIsGameFinished(packet->isGameEnded, packet->winningPlayerId);
		break;
	}
	case BasicNetworkMessages::SyncPlayers: {
		SyncPlayerListPacket* packet = (SyncPlayerListPacket*)payload;
		packet->SyncPlayerList(mPlayerList);
		break;
	}
	case  BasicNetworkMessages::ClientPlayerInputState: {
		ClientPlayerInputPacket* packet = (ClientPlayerInputPacket*)payload;
		HandleClientPlayerInputPacket(packet, source + 1);
		break;
	}
	case BasicNetworkMessages::ClientSyncItemSlot: {
		ClientSyncItemSlotPacket* packet = (ClientSyncItemSlotPacket*)(payload);
		HandlePlayerEquippedItemChange(packet);
		break;
	}
	case BasicNetworkMessages::SyncInteractable: {
		SyncInteractablePacket* packet = (SyncInteractablePacket*)(payload);
		HandleInteractablePacket(packet);
		break;
	}
	case BasicNetworkMessages::ClientSyncBuffs: {
		ClientSyncBuffPacket* packet = (ClientSyncBuffPacket*)(payload);
		HandlePlayerBuffChange(packet);
		break;
	}
	case BasicNetworkMessages::ClientSyncLocalActiveCause: {
		ClientSyncLocalActiveSusCausePacket* packet = (ClientSyncLocalActiveSusCausePacket*)(payload);
		HandleLocalActiveSusCauseChange(packet);
		break;
	}
	case BasicNetworkMessages::ClientSyncLocalSusChange: {
		ClientSyncLocalSusChangePacket* packet = (ClientSyncLocalSusChangePacket*)(payload);
		HandleLocalSusChange(packet);
		break;
	}
	case BasicNetworkMessages::ClientSyncGlobalSusChange: {
		ClientSyncGlobalSusChangePacket* packet = (ClientSyncGlobalSusChangePacket*)(payload);
		HandleGlobalSusChange(packet);
		break;
	}
	case BasicNetworkMessages::SyncObjectState: {
		SyncObjectStatePacket* packet = (SyncObjectStatePacket*)(payload);
		HandleObjectStatePacket(packet);
		break;
	}
	case BasicNetworkMessages::ClientInit: {
		ClientInitPacket* packet = (ClientInitPacket*)(payload);
		const int playerPeer = source + 1;
		HandleClientInitPacket(packet, playerPeer);
		break;
	}
	case BasicNetworkMessages::SyncPlayerIdNameMap: {
		const SyncPlayerIdNameMapPacket* packet = (SyncPlayerIdNameMapPacket*)(payload);
		HandleSyncPlayerIdNameMapPacket(packet);
		break;
	}
	case BasicNetworkMessages::SyncAnnouncements: {
		const AnnouncementSyncPacket* packet = (AnnouncementSyncPacket*)(payload);
		HandleAnnouncementSync(packet);
		break;
	}
	case BasicNetworkMessages::GuardSpotSound: {
		GuardSpotSoundPacket* packet = (GuardSpotSoundPacket*)(payload);
		HandleGuardSpotSound(packet);
		break;
	}
	case BasicNetworkMessages::DistributedClientConnectToPhysicsServer: {
		DistributedClientConnectToPhysicsServerPacket* packet = static_cast<DistributedClientConnectToPhysicsServerPacket*>(payload);
		HandleOnConnectToDistributedPhysicsServerPacketReceived(packet);
		break;
	}
	case BasicNetworkMessages::DistributedClientGetGameInstanceData: {
		DistributedClientGetGameInstanceDataPacket* packet = static_cast<DistributedClientGetGameInstanceDataPacket*>(payload);
		if (packet->peerID == mThisClient->GetPeerID() - 1) {
			if (!packet->isGameInstanceFound) {
				//TODO(erendgrmnc): return to main menu.
				break;
			}
			HandleOnDistributedGameClientGameInstanceDataPacketReceived(packet);
		}

		break;
	}
	default:
		std::cout << "Received unknown packet. Type: " << payload->type << std::endl;
		break;
	}
}

void MultiplayerGameScene::InitInGameMenuManager() {
	MultiplayerLobby* multiplayerLobby = new MultiplayerLobby(this);
	mPushdownMachine = new PushdownMachine(multiplayerLobby);
}

void MultiplayerGameScene::SendClientSyncItemSlotPacket(int playerNo, int invSlot, int inItem, int usageCount) const {
	PlayerInventory::item itemToEquip = (PlayerInventory::item)(inItem);
	NCL::CSC8503::ClientSyncItemSlotPacket packet(playerNo, invSlot, itemToEquip, usageCount);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SendClientSyncBuffPacket(int playerNo, int buffType, bool toApply) const {
	PlayerBuffs::buff buffToSync = (PlayerBuffs::buff)(buffType);
	NCL::CSC8503::ClientSyncBuffPacket packet(playerNo, buffToSync, toApply);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SendInteractablePacket(int networkObjectId, bool isOpen, int interactableItemType) const {
	SyncInteractablePacket packet(networkObjectId, isOpen, interactableItemType);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SendClientSyncLocalActiveSusCausePacket(int playerNo, int activeSusCause, bool toApply) const {
	LocalSuspicionMetre::activeLocalSusCause activeCause = (LocalSuspicionMetre::activeLocalSusCause)(activeSusCause);
	NCL::CSC8503::ClientSyncLocalActiveSusCausePacket packet(playerNo, activeCause, toApply);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SendClientSyncLocalSusChangePacket(int playerNo, int changedValue) const {
	NCL::CSC8503::ClientSyncLocalSusChangePacket packet(playerNo, changedValue);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SendClientSyncGlobalSusChangePacket(int changedValue) const {
	NCL::CSC8503::ClientSyncGlobalSusChangePacket packet(changedValue);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SendClientSyncLocationActiveSusCausePacket(int cantorPairedLocation, int activeSusCause, bool toApply) const {
	LocationBasedSuspicion::activeLocationSusCause activeCause = (LocationBasedSuspicion::activeLocationSusCause)(activeSusCause);
	NCL::CSC8503::ClientSyncLocationActiveSusCausePacket packet(cantorPairedLocation, activeCause, toApply);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SendClientSyncLocationSusChangePacket(int cantorPairedLocation, int changedValue) const {
	NCL::CSC8503::ClientSyncLocationSusChangePacket packet(cantorPairedLocation, changedValue);
	mThisServer->SendGlobalPacket(packet);
}

void NCL::CSC8503::MultiplayerGameScene::SendAnnouncementSyncPacket(int annType, float time, int playerNo) {
	NCL::CSC8503::AnnouncementSyncPacket packet(annType, time, playerNo);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SendGuardSpotSoundPacket(int playerId) const {
	GuardSpotSoundPacket packet(playerId);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SendPacketsThread() {
	while (mThisServer) {
		std::lock_guard<std::mutex> lock(mPacketToSendQueueMutex);
		if (mPacketToSendQueue.size() > 1 && !mPacketToSendQueue.empty()) {

			GamePacket* packet = mPacketToSendQueue.front();
			if (packet) {
				std::cout << "Sending Global Packet\n";
				mThisServer->SendGlobalPacket(*packet);
				mPacketToSendQueue.pop();
			}
		}
	}
}

void MultiplayerGameScene::SendGameClientConnectedToDistributedManagerPacket(int gameInstanceID) {
	std::cout << "Sending game client connect packet for game ID: " << gameInstanceID << "\n";
	DistributedClientConnectedToSystemPacket packet(gameInstanceID, DistributedSystemClientType::DistributedGameClient);
	mThisClient->SendPacket(packet);
}

GameClient* MultiplayerGameScene::GetClient() const {
	return mThisClient;
}

void MultiplayerGameScene::SendObjectStatePacket(int networkId, int state) const {
	NCL::CSC8503::SyncObjectStatePacket packet(networkId, state);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::ClearNetworkGame() {
	if (mThisClient) {
		mThisClient->ClearPacketHandlers();
	}
	else {
		mThisServer->ClearPacketHandlers();
	}

	mServerPlayers.clear();

	mLevelManager->ClearLevel();

	mWinningPlayerId = -1;
	mNetworkObjectCache = 10;

	for (auto& client : mDistributedPhysicsClients) {
		client.second->SetClientLastFullID(-1);
	}
}

GameServer* MultiplayerGameScene::GetServer() const {
	return mThisServer;
}

NetworkPlayer* MultiplayerGameScene::GetLocalPlayer() const {
	return static_cast<NetworkPlayer*>(mLocalPlayer);
}

void MultiplayerGameScene::UpdateAsServer(float dt) {
	mPacketsToSnapshot--;
	if (mPacketsToSnapshot < 0) {
		BroadcastSnapshot(false);
		mPacketsToSnapshot = 5;
	}
	else {
		BroadcastSnapshot(true);
	}
}

void MultiplayerGameScene::UpdateAsClient(float dt) {
	mThisClient->UpdateClient();

	for (const auto& client : mDistributedPhysicsClients) {
		client.second->UpdateClient();
	}
}

void MultiplayerGameScene::BroadcastSnapshot(bool deltaFrame) {
	std::vector<GameObject*>::const_iterator first;
	std::vector<GameObject*>::const_iterator last;

	mLevelManager->GetGameWorld()->GetObjectIterators(first, last);

	for (auto i = first; i != last; ++i) {
		NetworkObject* o = (*i)->GetNetworkObject();
		if (!o) {
			continue;
		}
		//TODO - you'll need some way of determining
		//when a player has sent the server an acknowledgement
		//and store the lastID somewhere. A map between player
		//and an int could work, or it could be part of a 
		//NetworkPlayer struct. 
		int playerState = o->GetLatestNetworkState().stateID;
		GamePacket* newPacket = nullptr;
		if (o->WritePacket(&newPacket, deltaFrame, mServerSideLastFullID)) {
			if (newPacket != nullptr) {
				std::lock_guard<std::mutex> lock(mPacketToSendQueueMutex);
				mPacketToSendQueue.push(newPacket);
			}
		}
	}
}

void MultiplayerGameScene::UpdateMinimumState() {
	//Periodically remove old data from the server
	int minID = INT_MAX;
	int maxID = 0; //we could use this to see if a player is lagging behind?

	for (auto i : mStateIDs) {
		minID = std::min(minID, i.second);
		maxID = std::max(maxID, i.second);
	}
	//every client has acknowledged reaching at least state minID
	//so we can get rid of any old states!
	std::vector<GameObject*>::const_iterator first;
	std::vector<GameObject*>::const_iterator last;
	mLevelManager->GetGameWorld()->GetObjectIterators(first, last);

	for (auto i = first; i != last; ++i) {
		NetworkObject* o = (*i)->GetNetworkObject();
		if (!o) {
			continue;
		}
		o->UpdateStateHistory(minID); //clear out old states so they arent taking up memory...
	}
}

int MultiplayerGameScene::GetPlayerPeerID(int peerId) {
	if (peerId == -2) {
		peerId = mThisClient->GetPeerID();
	}
	for (int i = 0; i < 4; ++i) {
		if (mPlayerList[i] == peerId) {
			return i;
		}
	}
	return -1;
}

void MultiplayerGameScene::SendStartGameStatusPacket(const std::string& seed) const {
	GameStartStatePacket state(mIsGameStarted, 0, seed);
	mThisServer->SendGlobalPacket(state);
}

void MultiplayerGameScene::SendFinishGameStatusPacket() {
	GameEndStatePacket packet(mIsGameFinished, mWinningPlayerId);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::InitWorld(const std::mt19937& levelSeed) {
	mLevelManager->GetGameWorld()->ClearAndErase();
	mLevelManager->GetPhysics()->Clear();

	mLevelManager->LoadLevel(mLocalPlayerId, mTotalPlayerCount, mObjectsPerPlayer);

	//SpawnPlayers();

	mLevelManager->SetPlayersForGuards();

	mLevelManager->InitAnimationSystemObjects();
}

void MultiplayerGameScene::HandleClientPlayerInput(ClientPlayerInputPacket* playerMovementPacket, int playerPeerID) {
	//TODO(erendgrmc)
}

void MultiplayerGameScene::SpawnPlayers() {

	for (int i = 0; i < 4; i++) {
		if (mPlayerList[i] != -1) {

			const Vector3 pos(0, 10, 0);///mLevelManager->GetPlayerStartPosition(i);
			auto* netPlayer = AddPlayerObject(pos, i);
			mServerPlayers.emplace(i, netPlayer);
			if (PlayerInventoryObserver* const inventoryObserver = reinterpret_cast<PlayerInventoryObserver*>(netPlayer)) {
				mLevelManager->GetInventoryBuffSystem()->GetPlayerInventoryPtr()->Attach(inventoryObserver);
			}

			if (PlayerBuffsObserver* const playerBuffsObserver = reinterpret_cast<PlayerBuffsObserver*>(netPlayer)) {
				mLevelManager->GetInventoryBuffSystem()->GetPlayerBuffsPtr()->Attach(playerBuffsObserver);
			}

		}
		else
		{
			mServerPlayers.emplace(i, nullptr);
		}
	}

	int playerPeerId = 0;

	if (!mThisServer) {
		playerPeerId = GetPlayerPeerID();
	}

	NetworkPlayer* localPlayer = mServerPlayers[playerPeerId];
	mLocalPlayer = localPlayer;

	mLocalPlayerId = mServerPlayers[playerPeerId]->GetPlayerID();
	localPlayer->SetIsLocalPlayer(true);
	mLevelManager->SetTempPlayer((PlayerObject*)mLocalPlayer);
	mLocalPlayer->ToggleIsRendered();
}

void MultiplayerGameScene::LogObjectPositionToTxtFile(const Vector3& position, int timeAsSecond) {
	std::ofstream file("object_positions.txt", std::ios::app); // Open file in append mode

	if (file.is_open()) {
		std::stringstream ss;
		ss << "(" << std::fixed << std::setprecision(6) << position.x << "), (" << std::fixed << std::setprecision(6) << position.y << "), (" << std::fixed << std::setprecision(6) << position.z << ") - (" << timeAsSecond << ")\n";
		file << ss.str();
		file.close();
	}
	else {
		std::cerr << "Error opening file for writing." << std::endl;
	}
}

NetworkPlayer* MultiplayerGameScene::AddPlayerObject(const Vector3& position, int playerNum) {

	//Set Player Obj Name
	char buffer[256]; // Adjust the size according to your needs
	strcpy_s(buffer, sizeof(buffer), _strdup(PLAYER_PREFIX));
	strcat_s(buffer, sizeof(buffer), std::to_string(playerNum).c_str());

	auto* netPlayer = new NetworkPlayer(this, playerNum, buffer);
	mLevelManager->CreatePlayerObjectComponents(*netPlayer, position);

	auto* networkComponet = new NetworkObject(*netPlayer, playerNum);
	netPlayer->SetNetworkObject(networkComponet);
	mNetworkObjects.push_back(netPlayer->GetNetworkObject());
	mLevelManager->GetGameWorld()->AddGameObject(netPlayer);
	mLevelManager->AddUpdateableGameObject(*netPlayer);
	Vector4 colour;
	switch (playerNum)
	{
	case 0:
		netPlayer->GetRenderObject()->SetMatTextures(mLevelManager->GetMeshMaterial("Player_Red"));
		break;
	case 1:
		netPlayer->GetRenderObject()->SetMatTextures(mLevelManager->GetMeshMaterial("Player_Blue"));
		break;
	case 2:
		netPlayer->GetRenderObject()->SetMatTextures(mLevelManager->GetMeshMaterial("Player_Yellow"));
		break;
	case 3:
		netPlayer->GetRenderObject()->SetMatTextures(mLevelManager->GetMeshMaterial("Player_Green"));
		break;
	default:
		break;
	}

	return netPlayer;
}

void MultiplayerGameScene::HandleFullPacket(FullPacket* fullPacket) {

	if (fullPacket->serverID <= -1 || mDistributedPhysicsClients[fullPacket->serverID]->GetClientLastFullID() > fullPacket->fullState.stateID) {
		std::cout << "Discarding full packet. ServerID: " << fullPacket->serverID << "| Client Side Last ID: " << mDistributedPhysicsClients[fullPacket->serverID]->GetClientLastFullID() << "| Full Packet ID: " << fullPacket->fullState.stateID << "\n";
		return;
	}

	for (int i = 0; i < mNetworkObjects.size(); i++) {
		if (mNetworkObjects[i]->GetnetworkID() == fullPacket->objectID) {

			mNetworkObjects[i]->ReadPacket(*fullPacket);
		}
	}
	mDistributedPhysicsClients[fullPacket->serverID]->SetClientLastFullID(fullPacket->fullState.stateID);
}

void MultiplayerGameScene::HandleDeltaPacket(DeltaPacket* deltaPacket) {

	for (int i = 0; i < mNetworkObjects.size(); i++) {
		if (mNetworkObjects[i]->GetnetworkID() == deltaPacket->objectID) {
			mNetworkObjects[i]->ReadPacket(*deltaPacket);
		}
	}
}

void MultiplayerGameScene::HandleClientPlayerInputPacket(ClientPlayerInputPacket* clientPlayerInputPacket, int playerPeerId) {
	int playerIndex = GetPlayerPeerID(playerPeerId);
	auto* playerToHandle = mServerPlayers[playerIndex];

	playerToHandle->SetPlayerInput(clientPlayerInputPacket->playerInputs);
	mServerSideLastFullID = clientPlayerInputPacket->lastId;
	UpdateMinimumState();
}

void MultiplayerGameScene::HandleAddPlayerScorePacket(AddPlayerScorePacket* packet) {
}

void MultiplayerGameScene::SyncPlayerList() {
	int peerId;
	mPlayerList[0] = 0;
	for (int i = 0; i < 3; ++i) {
		if (mThisServer->GetPeer(i, peerId)) {
			mPlayerList[i + 1] = peerId;
		}
		else {
			mPlayerList[i + 1] = -1;
		}
	}

	SyncPlayerListPacket packet(mPlayerList);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::SetItemsLeftToZero() {
}

void MultiplayerGameScene::HandlePlayerEquippedItemChange(ClientSyncItemSlotPacket* packet) const {
	const int localPlayerID = static_cast<NetworkPlayer*>(mLocalPlayer)->GetPlayerID();
	auto* inventorySystem = mLevelManager->GetInventoryBuffSystem()->GetPlayerInventoryPtr();
	const PlayerInventory::item equippedItem = static_cast<PlayerInventory::item>(packet->equippedItem);
	inventorySystem->ChangePlayerItem(packet->playerID, localPlayerID, packet->slotId, equippedItem, packet->usageCount);
}

void MultiplayerGameScene::HandleInteractablePacket(SyncInteractablePacket* packet) const {
	InteractableItems interactableItemType = static_cast<InteractableItems>(packet->interactableItemType);

	NetworkObject* interactedObj = nullptr;
	for (auto* networkObject : mNetworkObjects) {
		if (networkObject->GetnetworkID() == packet->networkObjId) {
			interactedObj = networkObject;
			break;
		}
	}

	switch (interactableItemType) {
	case InteractableItems::InteractableDoors: {
		InteractableDoor* doorObj = reinterpret_cast<InteractableDoor*>(interactedObj);
		doorObj->SyncDoor(packet->isOpen);
		break;
	}
	case InteractableItems::InteractableVents: {
		Vent* ventObj = reinterpret_cast<Vent*>(interactedObj);
		ventObj->SetIsOpen(packet->isOpen, false);
		break;
	}
	case InteractableItems::HeistItem: {
		LevelManager::GetLevelManager()->GetMainFlag()->Reset();
		break;
	}
	}
}
void MultiplayerGameScene::HandlePlayerBuffChange(ClientSyncBuffPacket* packet) const {
	const int localPlayerID = static_cast<NetworkPlayer*>(mLocalPlayer)->GetPlayerID();
	auto* buffSystem = mLevelManager->GetInventoryBuffSystem()->GetPlayerBuffsPtr();
	const PlayerBuffs::buff buffToSync = static_cast<PlayerBuffs::buff>(packet->buffID);
	buffSystem->SyncPlayerBuffs(packet->playerID, localPlayerID, buffToSync, packet->toApply);
}

void MultiplayerGameScene::HandleLocalActiveSusCauseChange(ClientSyncLocalActiveSusCausePacket* packet) const {
	const int localPlayerID = static_cast<NetworkPlayer*>(mLocalPlayer)->GetPlayerID();
	auto* localSusMetre = mLevelManager->GetSuspicionSystem()->GetLocalSuspicionMetre();
	const LocalSuspicionMetre::activeLocalSusCause activeCause = static_cast<LocalSuspicionMetre::activeLocalSusCause>(packet->activeLocalSusCauseID);
	localSusMetre->SyncActiveSusCauses(packet->playerID, localPlayerID, activeCause, packet->toApply);
}

void MultiplayerGameScene::HandleLocalSusChange(ClientSyncLocalSusChangePacket* packet) const {
	const int localPlayerID = static_cast<NetworkPlayer*>(mLocalPlayer)->GetPlayerID();
	auto* localSusMetre = mLevelManager->GetSuspicionSystem()->GetLocalSuspicionMetre();
	localSusMetre->SyncSusChange(packet->playerID, localPlayerID, packet->changedValue);
}

void MultiplayerGameScene::HandleGlobalSusChange(ClientSyncGlobalSusChangePacket* packet) const {
	auto* globalSusMetre = mLevelManager->GetSuspicionSystem()->GetGlobalSuspicionMetre();
	globalSusMetre->SyncSusChange(packet->changedValue);
}

void MultiplayerGameScene::HandleLocationActiveSusCauseChange(ClientSyncLocationActiveSusCausePacket* packet) const {
	const int localPlayerID = static_cast<NetworkPlayer*>(mLocalPlayer)->GetPlayerID();
	auto* locationSusMetre = mLevelManager->GetSuspicionSystem()->GetLocationBasedSuspicion();
	const LocationBasedSuspicion::activeLocationSusCause activeCause = static_cast<LocationBasedSuspicion::activeLocationSusCause>(packet->activeLocationSusCauseID);
	locationSusMetre->SyncActiveSusCauses(activeCause, packet->cantorPairedLocation, packet->toApply);
}

void MultiplayerGameScene::HandleLocationSusChange(ClientSyncLocationSusChangePacket* packet) const {
	auto* locationSusMetre = mLevelManager->GetSuspicionSystem()->GetLocationBasedSuspicion();
	locationSusMetre->SyncSusChange(packet->cantorPairedLocation, packet->changedValue);
}

void MultiplayerGameScene::HandleAnnouncementSync(const AnnouncementSyncPacket* packet) const {
	if (!mLocalPlayer)
		return;
	const PlayerObject::AnnouncementType annType = static_cast<PlayerObject::AnnouncementType>(packet->annType);
	GetLocalPlayer()->SyncAnnouncements(annType, packet->time, packet->playerNo);
}

void MultiplayerGameScene::HandleGuardSpotSound(GuardSpotSoundPacket* packet) const {
	if (packet->playerId == mLocalPlayerId) {
#ifndef DISTRIBUTEDSYSTEMACTIVE
		mLevelManager->GetSoundManager()->PlaySpottedSound();
#endif
	}
}

void MultiplayerGameScene::AddToPlayerPeerNameMap(int playerId, const std::string& playerName) {
	mPlayerPeerNameMap.emplace(std::pair(playerId, playerName));
	if (mThisServer) {
		WriteAndSendSyncPlayerIdNameMapPacket();
	}
}

void MultiplayerGameScene::HandleClientInitPacket(const ClientInitPacket* packet, int playerID) {
	AddToPlayerPeerNameMap(playerID, packet->playerName);
}

void MultiplayerGameScene::WriteAndSendSyncPlayerIdNameMapPacket() const {
	SyncPlayerIdNameMapPacket packet(mPlayerPeerNameMap);
	mThisServer->SendGlobalPacket(packet);
}

void MultiplayerGameScene::HandleSyncPlayerIdNameMapPacket(const SyncPlayerIdNameMapPacket* packet) {
	mPlayerPeerNameMap.clear();
	for (int i = 0; i < 4; i++) {
		if (packet->playerIds[i] != -1) {
			std::pair<int, std::string> playerIdNamePair(packet->playerIds[i], packet->playerNames[i]);
			mPlayerPeerNameMap.insert(playerIdNamePair);
		}
	}
}

void MultiplayerGameScene::HandleOnConnectToDistributedPhysicsServerPacketReceived(
	DistributedClientConnectToPhysicsServerPacket* packet) {

	std::vector<char> ipOctets = IpToCharArray(packet->ipAddress);
	std::cout << "Connecting to physics server on: " << packet->ipAddress << " | " << packet->physicsPacketDistributerPort << "\n";

	ConnectClientToDistributedGameServer(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], packet->physicsPacketDistributerPort, packet->physicsServerID, "");
}

void MultiplayerGameScene::HandleOnDistributedGameClientGameInstanceDataPacketReceived(
	DistributedClientGetGameInstanceDataPacket* packet) {
	mGameInstanceID = packet->gameInstanceID;
	mTotalPlayerCount = packet->playerCount;
	mObjectsPerPlayer = packet->objectsPerPlayer;
	mLocalPlayerId = packet->playerNumber;
}

bool MultiplayerGameScene::ConnectClientToDistributedGameServer(char a, char b, char c, char d, int port, int gameServerID,
	const std::string& playerName) {
	auto* client = new NCL::CSC8503::GameClient();
	const bool isConnected = client->Connect(a, b, c, d, port, playerName);

	if (isConnected) {
		client->RegisterPacketHandler(Delta_State, this);
		client->RegisterPacketHandler(Full_State, this);
		client->RegisterPacketHandler(Player_Connected, this);
		client->RegisterPacketHandler(Player_Disconnected, this);
		client->RegisterPacketHandler(String_Message, this);
	}

	std::pair<int, GameClient*> connectedClient = std::make_pair(gameServerID, client);
	mDistributedPhysicsClients.insert(connectedClient);

	Profiler::SetConnectedServerCount(Profiler::GetConnectedPhysicsServerCount() + 1);

	return isConnected;
}

std::vector<char> MultiplayerGameScene::IpToCharArray(const std::string& ipAddress) {
	std::vector<std::string> ip_bytes;
	std::stringstream ss(ipAddress);
	std::string segment;

	while (std::getline(ss, segment, '.')) {
		ip_bytes.push_back(segment);
	}

	if (ip_bytes.size() != 4) {
		throw std::invalid_argument("Invalid IPv4 address format");
	}

	std::vector<char> ip_packed;
	for (const std::string& byte_str : ip_bytes) {
		int byte_value = std::stoi(byte_str);
		if (byte_value < 0 || byte_value > 255) {
			throw std::invalid_argument("Invalid IPv4 address format");
		}
		ip_packed.push_back(static_cast<char>(byte_value));
	}

	return ip_packed;
}

void MultiplayerGameScene::ShowPlayerList() const {
	if (Window::GetKeyboard()->KeyDown(KeyCodes::TAB)) {
		Vector2 position(15, 20);

		for (const std::pair<int, std::string>& playerIdNamePair : mPlayerPeerNameMap) {
			const Vector4& textColour = playerIdNamePair.first == mLocalPlayerId ? LOCAL_PLAYER_COLOUR : DEFAULT_PLAYER_COLOUR;

			std::stringstream ss;
			ss << playerIdNamePair.second << " ------- (" << playerIdNamePair.first << ")";
			Debug::Print(ss.str(), position, textColour);
			position.y += VERTICAL_MARGIN_BETWEEN_PLAYER_NAMES;
		}
	}
}

void MultiplayerGameScene::DrawCanvas() {
	if (ImGui::CollapsingHeader("Distributed Client Attributes"))
	{
		ImGui::BeginTable("Client Attributes", 2);

		ImGui::TableNextColumn();
		ImGui::Text("FPS");
		ImGui::TableNextColumn();
		const std::string fpsStr = std::to_string(Profiler::GetFramesPerSecond());

		ImGui::Text(fpsStr.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Network Time");
		ImGui::TableNextColumn();
		std::string objInServerStr = std::to_string(Profiler::GetNetworkTime()) + "ms";
		ImGui::Text(objInServerStr.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Connected Distributed Game Servers");
		ImGui::TableNextColumn();
		std::string connectedServerCount = std::to_string(Profiler::GetConnectedPhysicsServerCount());
		ImGui::Text(connectedServerCount.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Time passed per update");
		ImGui::TableNextColumn();
		std::string timePassedPerUpdateStr = std::to_string(Profiler::GetTimePassedPerUpdate()) + "ms";
		ImGui::Text(timePassedPerUpdateStr.c_str());


		ImGui::TableNextColumn();

		ImGui::TableNextColumn();
		ImGui::TableNextColumn();
		ImGui::TableNextColumn();


		ImGui::EndTable();
	}

	if (ImGui::CollapsingHeader("Memory Usage"))
	{
		ImGui::BeginTable("Memory Usage Table", 2);

		ImGui::TableNextColumn();
		ImGui::Text("Virtual Memory By Program");
		ImGui::TableNextColumn();
		const std::string& memStr = Profiler::GetVirtualMemoryUsageByProgram();

		ImGui::Text(memStr.c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Virtual Memory");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetVirtualMemoryUsage().c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Total Virtual Memory");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetTotalVirtualMemory().c_str());

		ImGui::TableNextColumn();
		ImGui::TableNextColumn();
		ImGui::TableNextColumn();

		ImGui::Text("Physical Memory By Program");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetPhysicalMemoryUsageByProgram().c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Physical Memory");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetPhysicalMemoryUsage().c_str());

		ImGui::TableNextColumn();

		ImGui::Text("Total Physical Memory");
		ImGui::TableNextColumn();
		ImGui::Text(Profiler::GetTotalPhysicalMemory().c_str());

		ImGui::EndTable();
	}
}

void MultiplayerGameScene::HandleObjectStatePacket(SyncObjectStatePacket* packet) const {
	NetworkObject* objectToChangeState = nullptr;
	for (NetworkObject* networkObject : mNetworkObjects) {
		if (networkObject->GetnetworkID() == packet->networkObjId) {
			objectToChangeState = networkObject;
			break;
		}
	}

	GameObject::GameObjectState state = static_cast<GameObject::GameObjectState>(packet->objectState);
	if (objectToChangeState != nullptr) {
		objectToChangeState->GetGameObject().SetObjectState(state);
	}
}
#endif
