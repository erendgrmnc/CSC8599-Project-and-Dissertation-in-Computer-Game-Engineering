#include "PhysicsObject.h"
#ifdef USEGL
#include "NetworkObject.h"
#include "./enet/enet.h"

#include <algorithm>
#include <iostream>
using namespace NCL;
using namespace CSC8503;

SyncPlayerListPacket::SyncPlayerListPacket(std::vector<int>& serverPlayers) {
	type = BasicNetworkMessages::SyncPlayers;
	size = sizeof(SyncPlayerListPacket);

	for (int i = 0; i < 4; i++) {
		playerList[i] = serverPlayers[i];
	}
}

void SyncPlayerListPacket::SyncPlayerList(std::vector<int>& clientPlayerList) const
{
	//TODO(erendgrmnc): Add config for max player number.

	for (int i = 0; i < 4; ++i) {
		clientPlayerList[i] = playerList[i];
	}
}

GameStartStatePacket::GameStartStatePacket(bool val, int gameInstanceId, const std::string& seed) {
	type = BasicNetworkMessages::GameStartState;
	size = sizeof(GameStartStatePacket);

	isGameStarted = val;
	this->gameInstanceId = gameInstanceId;
	CopyToPacketField(this->levelSeed, seed);
}

GameEndStatePacket::GameEndStatePacket(bool val, int winningPlayerId) {
	type = BasicNetworkMessages::GameEndState;
	size = sizeof(GameEndStatePacket);

	this->isGameEnded = val;
	this->winningPlayerId = winningPlayerId;
}

ClientPlayerInputPacket::ClientPlayerInputPacket(int lastId, const PlayerInputs& playerInputs) {
	type = BasicNetworkMessages::ClientPlayerInputState;
	size = sizeof(ClientPlayerInputPacket);

	this->playerInputs.isCrouching = playerInputs.isCrouching;
	this->playerInputs.isSprinting = playerInputs.isSprinting;
	this->playerInputs.isEquippedItemUsed = playerInputs.isEquippedItemUsed;
	this->playerInputs.isInteractButtonPressed = playerInputs.isInteractButtonPressed;
	this->playerInputs.isHoldingInteractButton = playerInputs.isHoldingInteractButton;

	this->playerInputs.leftHandItemId = playerInputs.leftHandItemId;
	this->playerInputs.rightHandItemId = playerInputs.rightHandItemId;

	this->playerInputs.movementButtons[0] = playerInputs.movementButtons[0];
	this->playerInputs.movementButtons[1] = playerInputs.movementButtons[1];
	this->playerInputs.movementButtons[2] = playerInputs.movementButtons[2];
	this->playerInputs.movementButtons[3] = playerInputs.movementButtons[3];

	this->playerInputs.fwdAxis.x = playerInputs.fwdAxis.x;
	this->playerInputs.fwdAxis.y = playerInputs.fwdAxis.y;
	this->playerInputs.fwdAxis.z = playerInputs.fwdAxis.z;

	this->playerInputs.rightAxis.x = playerInputs.rightAxis.x;
	this->playerInputs.rightAxis.y = playerInputs.rightAxis.y;
	this->playerInputs.rightAxis.z = playerInputs.rightAxis.z;
	this->playerInputs.cameraYaw = playerInputs.cameraYaw;

	this->playerInputs.rayFromPlayer = playerInputs.rayFromPlayer;

	this->lastId = lastId;
	this->mouseXLook = mouseXLook;
}

ClientPlayerInputPacket::ClientPlayerInputPacket(int lastId, int playerID, const PlayerInputs& playerInputs) {
	type = BasicNetworkMessages::ClientPlayerInputState;
	size = sizeof(ClientPlayerInputPacket);

	this->playerID = playerID;

	this->playerInputs.isCrouching = playerInputs.isCrouching;
	this->playerInputs.isSprinting = playerInputs.isSprinting;
	this->playerInputs.isEquippedItemUsed = playerInputs.isEquippedItemUsed;
	this->playerInputs.isInteractButtonPressed = playerInputs.isInteractButtonPressed;
	this->playerInputs.isHoldingInteractButton = playerInputs.isHoldingInteractButton;

	this->playerInputs.leftHandItemId = playerInputs.leftHandItemId;
	this->playerInputs.rightHandItemId = playerInputs.rightHandItemId;

	this->playerInputs.movementButtons[0] = playerInputs.movementButtons[0];
	this->playerInputs.movementButtons[1] = playerInputs.movementButtons[1];
	this->playerInputs.movementButtons[2] = playerInputs.movementButtons[2];
	this->playerInputs.movementButtons[3] = playerInputs.movementButtons[3];

	this->playerInputs.fwdAxis.x = playerInputs.fwdAxis.x;
	this->playerInputs.fwdAxis.y = playerInputs.fwdAxis.y;
	this->playerInputs.fwdAxis.z = playerInputs.fwdAxis.z;

	this->playerInputs.rightAxis.x = playerInputs.rightAxis.x;
	this->playerInputs.rightAxis.y = playerInputs.rightAxis.y;
	this->playerInputs.rightAxis.z = playerInputs.rightAxis.z;
	this->playerInputs.cameraYaw = playerInputs.cameraYaw;

	this->playerInputs.rayFromPlayer = playerInputs.rayFromPlayer;

	this->lastId = lastId;
	this->mouseXLook = mouseXLook;
}

ClientUseItemPacket::ClientUseItemPacket(int objectID, int playerID) {
	this->objectID = objectID;
	this->playerID = playerID;
}

ClientSyncBuffPacket::ClientSyncBuffPacket(int playerID, int buffID, bool toApply) {
	type = BasicNetworkMessages::ClientSyncBuffs;
	size = sizeof(ClientSyncBuffPacket);

	this->playerID = playerID;
	this->buffID = buffID;
	this->toApply = toApply;
}

ClientSyncLocalActiveSusCausePacket::ClientSyncLocalActiveSusCausePacket(int playerID, int activeLocalSusCauseID, bool toApply) {
	type = BasicNetworkMessages::ClientSyncLocalActiveCause;
	size = sizeof(ClientSyncLocalActiveSusCausePacket);

	this->playerID = playerID;
	this->activeLocalSusCauseID = activeLocalSusCauseID;
	this->toApply = toApply;
}

ClientSyncLocalSusChangePacket::ClientSyncLocalSusChangePacket(int playerID, int changedValue) {
	type = BasicNetworkMessages::ClientSyncLocalSusChange;
	size = sizeof(ClientSyncLocalSusChangePacket);

	this->playerID = playerID;
	this->changedValue = changedValue;
}

ClientSyncGlobalSusChangePacket::ClientSyncGlobalSusChangePacket(int changedValue) {
	type = BasicNetworkMessages::ClientSyncGlobalSusChange;
	size = sizeof(ClientSyncGlobalSusChangePacket);

	this->changedValue = changedValue;
}

ClientSyncLocationActiveSusCausePacket::ClientSyncLocationActiveSusCausePacket(int cantorPairedLocation, int activeLocationSusCauseID, bool toApply) {
	type = BasicNetworkMessages::ClientSyncLocationActiveCause;
	size = sizeof(ClientSyncLocationActiveSusCausePacket);

	this->cantorPairedLocation = cantorPairedLocation;
	this->activeLocationSusCauseID = activeLocationSusCauseID;
	this->toApply = toApply;
}

ClientSyncLocationSusChangePacket::ClientSyncLocationSusChangePacket(int cantorPairedLocation, int changedValue) {
	type = BasicNetworkMessages::ClientSyncLocationSusChange;
	size = sizeof(ClientSyncLocationSusChangePacket);

	this->cantorPairedLocation = cantorPairedLocation;
	this->changedValue = changedValue;
}

ClientSyncItemSlotUsagePacket::ClientSyncItemSlotUsagePacket(int playerID, int firstItemUsage, int secondItemUsage) {
	this->firstItemUsage = firstItemUsage;
	this->secondItemUsage = secondItemUsage;
	this->playerID = playerID;
}

ClientSyncItemSlotPacket::ClientSyncItemSlotPacket(int playerID, int slotId, int equippedItem, int usageCount) {
	type = BasicNetworkMessages::ClientSyncItemSlot;
	size = sizeof(ClientSyncItemSlotPacket);

	this->playerID = playerID;
	this->slotId = slotId;
	this->equippedItem = equippedItem;
	this->usageCount = usageCount;
}

SyncInteractablePacket::SyncInteractablePacket(int networkObjectId, bool isOpen, int interactableItemType) {
	type = BasicNetworkMessages::SyncInteractable;
	size = sizeof(SyncInteractablePacket);

	this->networkObjId = networkObjectId;
	this->isOpen = isOpen;
	this->interactableItemType = interactableItemType;
}

SyncObjectStatePacket::SyncObjectStatePacket(int networkObjId, int objectState) {
	type = SyncObjectState;
	size = sizeof(SyncObjectStatePacket);

	this->networkObjId = networkObjId;
	this->objectState = objectState;
}

ClientInitPacket::ClientInitPacket(const std::string& playerName) {
	type = ClientInit;
	size = sizeof(ClientInitPacket);

	this->playerName = playerName;
}

SyncPlayerIdNameMapPacket::SyncPlayerIdNameMapPacket(const std::map<int, std::string>& playerIdNameMap) {
	type = SyncPlayerIdNameMap;
	size = sizeof(SyncPlayerIdNameMapPacket);

	int counter = 0;
	for (std::pair<int, std::string> playerIdName : playerIdNameMap) {
		playerIds[counter] = playerIdName.first;
		playerNames[counter] = playerIdName.second;
		counter++;
	}

}

AnnouncementSyncPacket::AnnouncementSyncPacket(int annType, float time, int playerNo) {
	type = SyncAnnouncements;
	size = sizeof(AnnouncementSyncPacket);

	this->annType = annType;
	this->time = time;
	this->playerNo = playerNo;
}

GuardSpotSoundPacket::GuardSpotSoundPacket(const int playerId) {
	type = BasicNetworkMessages::GuardSpotSound;
	size = sizeof(GuardSpotSoundPacket);
	this->playerId = playerId;
}

DistributedClientConnectedToSystemPacket::DistributedClientConnectedToSystemPacket(
	int gameInstanceID, DistributedSystemClientType clientType) {
	type = BasicNetworkMessages::DistributedClientConnectedToManager;
	size = sizeof(DistributedClientConnectedToSystemPacket);

	this->gameInstanceID = gameInstanceID;
	this->distributedClientType = clientType;
}

DistributedClientGetGameInstanceDataPacket::DistributedClientGetGameInstanceDataPacket(bool isGameInstanceFound, int gameInstanceID,
	int playerNumber) {
	type = BasicNetworkMessages::DistributedClientGetGameInstanceData;
	size = sizeof(DistributedClientGetGameInstanceDataPacket);

	this->isGameInstanceFound = isGameInstanceFound;
	this->gameInstanceID = gameInstanceID;
	this->playerNumber = playerNumber;
}

DistributedClientGetGameInstanceDataPacket::DistributedClientGetGameInstanceDataPacket() : isGameInstanceFound(false),
gameInstanceID(-1),
playerNumber(-1) {
	type = BasicNetworkMessages::DistributedClientGetGameInstanceData;
	size = sizeof(DistributedClientGetGameInstanceDataPacket);
}

DistributedPhysicsClientConnectedToManagerPacket::DistributedPhysicsClientConnectedToManagerPacket(int port, int physicsServerID, int gameInstanceID, const std::string& ipAddress) {
	type = BasicNetworkMessages::DistributedPhysicsClientConnectedToManager;
	size = sizeof(DistributedPhysicsClientConnectedToManagerPacket);

	this->physicsPacketDistributorPort = port;
	this->physicsServerID = physicsServerID;
	this->gameInstanceID = gameInstanceID;
	CopyToPacketField(this->ipAddress, ipAddress);
}

DistributedClientConnectToPhysicsServerPacket::DistributedClientConnectToPhysicsServerPacket(int port, int physicsServerID, const std::string& ipAddress, const std::string& borderStr) {
	type = BasicNetworkMessages::DistributedClientConnectToPhysicsServer;
	size = sizeof(DistributedClientConnectToPhysicsServerPacket);

	this->physicsPacketDistributorPort = port;
	this->physicsServerID = physicsServerID;
	CopyToPacketField(this->ipAddress, ipAddress);

	strncpy(this->borderStr, borderStr.c_str(), sizeof(this->borderStr) - 1);
	this->borderStr[sizeof(this->borderStr) - 1] = '\0';
}

DistributedPhysicsServerAllClientsAreConnectedPacket::DistributedPhysicsServerAllClientsAreConnectedPacket(int gameInstanceID, int gameServerID, bool isGameServerReady) {
	type = BasicNetworkMessages::DistributedPhysicsServerAllClientsAreConnected;
	// Minus the header, like every other packet. It was the whole struct, so four
	// extra bytes went on the wire.
	size = sizeof(DistributedPhysicsServerAllClientsAreConnectedPacket) - sizeof(GamePacket);

	this->isGameServerReady = isGameServerReady;
	this->gameServerID = gameServerID;
	// Was never assigned, so the manager read whatever was on the stack where the
	// packet landed - 0xCCCCCCCC in a debug build. See the note in
	// SystemManager::CheckIsGameStartable: bootstrap depended on that garbage.
	this->gameInstanceID = gameInstanceID;
}

DistributedClientsGameServersAreReadyPacket::DistributedClientsGameServersAreReadyPacket() {
	type = BasicNetworkMessages::DistributedClientsGameServersAreReady;
	size = sizeof(DistributedClientsGameServersAreReadyPacket);

	// Fixed arrays are not zeroed by default, so an unset slot would put whatever
	// was on the stack onto the wire.
	for (int i = 0; i < 2; ++i) {
		ipAddresses[i][0] = 0;
		ports[i] = 0;
	}
}

StartDistributedGameServerPacket::StartDistributedGameServerPacket(int serverManagerPort, int gameInstanceID, int maxClientCount, int objectsPerPlayer, std::vector<int> serverPorts, std::vector<std::string> serverIps, std::vector<int> connectedServerIds, const std::map<int, const std::string>& serverBorderMap) {
	type = BasicNetworkMessages::StartDistributedPhysicsServer;
	size = sizeof(StartDistributedGameServerPacket);

	this->serverManagerPort = serverManagerPort;
	this->gameInstanceID = gameInstanceID;

	// Clamped, not trusted: every array in this packet is a fixed 20 entries, so an
	// instance with more servers than that would write past the end of the struct
	// and corrupt whatever follows it on the wire.
	this->totalServerCount = static_cast<int>(serverBorderMap.size());
	this->currentServerCount = static_cast<int>(serverIps.size());
	if (this->totalServerCount > MAX_SERVERS || this->currentServerCount > MAX_SERVERS) {
		std::cout << "ERROR: instance has " << this->totalServerCount << " servers and "
			<< this->currentServerCount << " registered, but the packet holds at most "
			<< MAX_SERVERS << ". Clamping - the instance will be incomplete.\n";
		this->totalServerCount = std::min(this->totalServerCount, MAX_SERVERS);
		this->currentServerCount = std::min(this->currentServerCount, MAX_SERVERS);
	}
	this->clientsToConnect = maxClientCount;
	this->objectsPerPlayer = objectsPerPlayer;

	for (int i = 0; i < totalServerCount; i++) {
		serverIDs[i] = i;
		const std::string& str = serverBorderMap.at(i);

		auto it = serverBorderMap.find(i);
		if (it != serverBorderMap.end()) {
			char arr[256];
			strcpy(arr, it->second.c_str());

			for (int j = 0; j < 256; j++) {
				this->borders[i][j] = arr[j];
			}
		}
		else {
			for (int j = 0; j < 256; j++) {
				this->borders[i][j] = '\0';// Handle missing key
			}
		}
	}

	for (int i = 0; i < currentServerCount; i++) {
		this->serverPorts[i] = serverPorts[i];
		CopyToPacketField(this->createdServerIPs[i], serverIps[i]);
		// -1 rather than i: a receiver must never fall back to treating the array
		// index as a server id, which is the bug this field exists to fix.
		this->connectedServerIDs[i] =
			(i < static_cast<int>(connectedServerIds.size())) ? connectedServerIds[i] : -1;
	}
}

StartSimulatingObjectPacket::StartSimulatingObjectPacket(int objectID, int newServerID, int senderServerID, NetworkState lastFullState, PhysicsObject& physicsObj) {
	type = BasicNetworkMessages::StartSimulatingObjectInServer;
	size = sizeof(StartSimulatingObjectPacket);

	std::cout << "Start simulation packet position: " << lastFullState.position << "\n";

	this->lastFullState.position = lastFullState.position;
	this->lastFullState.orientation = lastFullState.orientation;
	this->lastFullState.stateID = lastFullState.stateID;

	this->lastFullState.predictedPosition = physicsObj.GetTransform()->GetPredictedPosition();
	this->lastFullState.predictedOrientation = physicsObj.GetTransform()->GetPredictedOrientation();;

	// Defaults; the caller overwrites these when the object is under control. Set
	// here so an uncontrolled object never carries stale axis state across a handoff.
	this->mControllerPlayerID = -1;
	this->mMoveAxis = Vector3(0, 0, 0);
	this->mSenderTick = 0;
	// Cube. Overwritten by the sender from its archetype map; the default is a shape
	// rather than a sentinel because a receiver must always be able to build
	// something - a handoff that arrives with no usable archetype would otherwise
	// lose the object, which is worse than getting its shape wrong.
	this->mArchetypeID = 0;

	this->newOwnerServerID = newServerID;
	this->senderServerID = senderServerID;
	this->objectID = objectID;

	this->mAngularVelocity = physicsObj.GetAngularVelocity();
	this->mInverseInertiaTensor = physicsObj.GetInverseInertiaTensor();
	this->mInverseInertia = physicsObj.GetInverseInertia();
	this->mTorque = physicsObj.GetTorque();

	this->mForce = physicsObj.GetForce();
	this->mLinearVelocity = physicsObj.GetLinearVelocity();
}

StartSimulatingObjectReceivedPacket::StartSimulatingObjectReceivedPacket(int objectID, int newOwnerServerID) {
	type = BasicNetworkMessages::StartSimulatingObjectInServerReceived;
	size = sizeof(StartSimulatingObjectReceivedPacket);

	this->objectID = objectID;
	this->newOwnerServerID = newOwnerServerID;
}

RunDistributedPhysicsServerInstancePacket::RunDistributedPhysicsServerInstancePacket(int serverID,
	int gameInstanceID, int midwareID, std::string borderData) {
	type = BasicNetworkMessages::RunDistributedPhysicsServerInstance;
	size = sizeof(RunDistributedPhysicsServerInstancePacket);

	this->serverID = serverID;
	this->gameInstanceID = gameInstanceID;
	this->midwareID = midwareID;

	strcpy(this->borderStr, borderData.c_str());
}

PhysicsServerMiddlewareConnectedPacket::PhysicsServerMiddlewareConnectedPacket(const std::string& ipAddress) {
	type = BasicNetworkMessages::PhysicsServerMiddlewareConnected;
	size = sizeof(PhysicsServerMiddlewareConnectedPacket);

	CopyToPacketField(this->ipAddress, ipAddress);
}

PhysicsServerMiddlewareDataPacket::PhysicsServerMiddlewareDataPacket(int peerID, int middlewareID) {

	type = BasicNetworkMessages::PhysicsServerMiddlewareData;
	size = sizeof(PhysicsServerMiddlewareDataPacket);

	this->peerID = peerID;
	this->middlewareID = middlewareID;
}

NetworkObject::NetworkObject(GameObject& o, int id) : object(o) {
	deltaErrors = 0;
	fullErrors = 0;
	networkID = id;
}

NetworkObject::~NetworkObject() {
}

bool NetworkObject::ReadPacket(GamePacket& p) {
	// read packet depending on packet type
	// if neither it returns false

	if (p.type == Delta_State)
		return ReadDeltaPacket((DeltaPacket&)p);

	if (p.type == Full_State)
		return ReadFullPacket((FullPacket&)p);

	return false; //this isn't a packet we care about!
}

bool NetworkObject::WritePacket(GamePacket** p, bool deltaFrame, int stateID) {
	if (deltaFrame) {
		if (!WriteDeltaPacket(p, stateID)) {
			return WriteFullPacket(p);
		}
		return true;
	}
	return WriteFullPacket(p);
}

bool NetworkObject::WritePacket(GamePacket** p, bool deltaFrame, int stateID, int gameServerID) {
	if (deltaFrame) {
		if (!WriteDeltaPacket(p, stateID, gameServerID)) {
			return WriteFullPacket(p, gameServerID);
		}
		return true;
	}
	return WriteFullPacket(p, gameServerID);
}

//Client objects recieve these packets
bool NetworkObject::ReadDeltaPacket(DeltaPacket& p) {
	// Resolve the state this delta is encoded against. Matching only against the
	// NEWEST full state is too strict: the server encodes deltas against the oldest
	// state still unacknowledged across all clients, so any client running ahead of
	// the slowest one would reject every delta it was sent. The full states we have
	// already applied are retained in stateHistory precisely so we can look one up.
	NetworkState baseState;
	if (p.fullID == lastFullState.stateID) {
		baseState = lastFullState;
	}
	else if (!GetNetworkState(p.fullID, baseState)) {
		// The base state has already been pruned, or was never received.
		return false;
	}

	Vector3 fullPos = baseState.position;
	Vector3 predictedPos = baseState.predictedPosition;

	Quaternion fullOrientation = baseState.orientation;

	fullPos.x += p.pos[0];
	fullPos.y += p.pos[1];
	fullPos.z += p.pos[2];

	fullOrientation.x += ((float)p.orientation[0]) / 127.0f;
	fullOrientation.y += ((float)p.orientation[1]) / 127.0f;
	fullOrientation.z += ((float)p.orientation[2]) / 127.0f;
	fullOrientation.w += ((float)p.orientation[3]) / 127.0f;

	object.GetTransform().SetPosition(fullPos);
	object.GetTransform().SetOrientation(fullOrientation);
	object.GetTransform().SetPredictedPosition(predictedPos);

	// Anything older than the state the server is still encoding against can go.
	// Done after the delta is applied, so the base state is not pruned out from
	// under this call.
	UpdateStateHistory(p.fullID);
	return true;
}

bool NetworkObject::ReadFullPacket(FullPacket& p) {
	// if packet is old discard
	if (p.fullState.stateID < lastFullState.stateID) {
		std::cout << "Discarding Packet\n";
		return false;
	}

	lastFullState = p.fullState;

	object.GetTransform().SetPosition(lastFullState.position);
	object.GetTransform().SetOrientation(lastFullState.orientation);
	object.GetTransform().SetPredictedPosition(lastFullState.predictedPosition);
	object.GetTransform().SetPredictedOrientation(lastFullState.predictedOrientation);
	object.SetServerID(p.serverID);

	stateHistory.emplace_back(lastFullState);
	TrimStateHistory();

	return true;
}

bool NetworkObject::WriteDeltaPacket(GamePacket** p, int stateID) {
	DeltaPacket* dp = new DeltaPacket();
	NetworkState state;

	// if we cant get network objects state we fail
	if (!GetNetworkState(stateID, state))
		return false;

	// tells packet what state it is a delta of
	dp->fullID = stateID;
	dp->objectID = networkID;

	Vector3 currentPos = object.GetTransform().GetPosition();
	Quaternion currentOrientation = object.GetTransform().GetOrientation();

	// find difference between current game states orientation + position and the selected states orientation + position
	currentPos -= state.position;
	currentOrientation -= state.orientation;

	dp->pos[0] = (char)currentPos.x;
	dp->pos[1] = (char)currentPos.y;
	dp->pos[2] = (char)currentPos.z;

	dp->orientation[0] = (char)(currentOrientation.x * 127.0f);
	dp->orientation[1] = (char)(currentOrientation.y * 127.0f);
	dp->orientation[2] = (char)(currentOrientation.z * 127.0f);
	dp->orientation[3] = (char)(currentOrientation.w * 127.0f);
	*p = dp;

	return true;
}

bool NetworkObject::WriteFullPacket(GamePacket** p) {
	FullPacket* fp = new FullPacket();


	fp->objectID = networkID;
	fp->fullState.position = object.GetTransform().GetPosition();
	fp->fullState.orientation = object.GetTransform().GetOrientation();
	fp->fullState.predictedPosition = object.GetTransform().GetPredictedPosition();
	fp->fullState.predictedOrientation = object.GetTransform().GetPredictedOrientation();
	fp->fullState.stateID = lastFullState.stateID++;

	stateHistory.emplace_back(fp->fullState);
	*p = fp;

	return true;
}

bool NetworkObject::WriteFullPacket(GamePacket** p, int gameServerID) {
	FullPacket* fp = new FullPacket();

	fp->objectID = networkID;
	fp->fullState.position = object.GetTransform().GetPosition();
	fp->fullState.orientation = object.GetTransform().GetOrientation();
	fp->fullState.predictedPosition = object.GetTransform().GetPredictedPosition();
	fp->fullState.predictedOrientation = object.GetTransform().GetPredictedOrientation();
	fp->fullState.stateID = lastFullState.stateID++;
	fp->serverID = gameServerID;
	stateHistory.emplace_back(fp->fullState);
	TrimStateHistory();
	*p = fp;

	return true;
}

bool NetworkObject::WriteDeltaPacket(GamePacket** p, int stateID, int gameServerID) {
	DeltaPacket* dp = new DeltaPacket();
	NetworkState state;

	// if we cant get network objects state we fail
	if (!GetNetworkState(stateID, state))
		return false;

	// tells packet what state it is a delta of
	dp->fullID = stateID;
	dp->objectID = networkID;

	Vector3 currentPos = object.GetTransform().GetPosition();
	Quaternion currentOrientation = object.GetTransform().GetOrientation();

	// find difference between current game states orientation + position and the selected states orientation + position
	currentPos -= state.position;
	currentOrientation -= state.orientation;

	dp->pos[0] = (char)currentPos.x;
	dp->pos[1] = (char)currentPos.y;
	dp->pos[2] = (char)currentPos.z;

	dp->orientation[0] = (char)(currentOrientation.x * 127.0f);
	dp->orientation[1] = (char)(currentOrientation.y * 127.0f);
	dp->orientation[2] = (char)(currentOrientation.z * 127.0f);
	dp->orientation[3] = (char)(currentOrientation.w * 127.0f);

	dp->serverID = gameServerID;
	*p = dp;

	return true;
}

NetworkState& NetworkObject::GetLatestNetworkState() {
	return lastFullState;
}

void NetworkObject::SetLatestNetworkState(NetworkState& lastState) {
	lastFullState = lastState;
	stateHistory.push_back(lastFullState);
	TrimStateHistory();
}

void NetworkObject::FinishTransitionToNewServer(int newServerID) {
	mNewServerID = newServerID;
	mIsActualPosOutServer = true;
	mIsWaitingHandshake = true;
}

void NetworkObject::HandleTransitionComplete() {
	mIsActualPosOutServer = false;
	mIsPredictionInfoSent = false;
	mNewServerID = -1;
}

void NetworkObject::OnTransitionHandshakeReceived() {
	mIsWaitingHandshake = false;
}

void NetworkObject::AddReceivedObjectLastPacket(const NetworkState& state) {
	stateHistory.push_back(state);
}

bool NetworkObject::GetIsActualPosOutOfServer() const {
	return mIsActualPosOutServer;
}

bool NetworkObject::GetIsPredictionInfoSent() const {
	return mIsPredictionInfoSent;
}

bool NetworkObject::GetNetworkState(int stateID, NetworkState& state) {

	// get a state ID from state history if needed
	for (auto i = stateHistory.begin(); i < stateHistory.end(); ++i) {
		if ((*i).stateID == stateID) {
			state = (*i);
			//std::cout << "Successfully found network state.State ID: " << stateID << "\n";
			return true;

		}
	}
	//std::cout << "Couldn't find state for ID: " << stateID << ", stateHistorySize: " << stateHistory.size() << "\n";
	return false;
}

int NetworkObject::GetNewServerID() const {
	return mNewServerID;
}

// Backstop against unbounded growth. Normal pruning is driven by client
// acknowledgements (UpdateStateHistory), but a server with no connected clients, or
// a client whose deltas are all being rejected, never prunes - and stateHistory then
// grows for the whole run, one entry per object per full snapshot. That is a leak in
// its own right and it also inflates the linear scan in GetNetworkState.
//
// The cap is generous relative to the 10Hz full-snapshot rate: several seconds of
// history, far more than any in-flight delta can reference.
void NetworkObject::TrimStateHistory() {
	constexpr size_t kMaxStateHistory = 64;
	if (stateHistory.size() <= kMaxStateHistory) {
		return;
	}
	const size_t excess = stateHistory.size() - kMaxStateHistory;
	stateHistory.erase(stateHistory.begin(), stateHistory.begin() + excess);
}

void NetworkObject::UpdateStateHistory(int minID) {
	// once a client has accepted a delta packet or a network state has been
	// recieved then we can clear past state histories as they are not needed
	for (auto i = stateHistory.begin(); i < stateHistory.end();) {
		if ((*i).stateID < minID) {
			//std::cout << "Removing State: " << i->stateID << "\n";
			i = stateHistory.erase(i);
		}
		else
			++i;
	}
}

DistributedClientCommandPacket::DistributedClientCommandPacket(int commandType, int sequence,
	int hintServerID, const NCL::Interaction::CommandArgs& args) {
	type = BasicNetworkMessages::DistributedClientCommand;
	size = sizeof(DistributedClientCommandPacket) - sizeof(GamePacket);

	this->commandType = commandType;
	this->sequence = sequence;
	this->hintServerID = hintServerID;
	this->args = args;
}

DistributedCommandAckPacket::DistributedCommandAckPacket(int sequence, int playerID,
	int targetObjectID, int result, int correctedServerID) {
	type = BasicNetworkMessages::DistributedCommandAck;
	size = sizeof(DistributedCommandAckPacket) - sizeof(GamePacket);

	this->sequence = sequence;
	this->playerID = playerID;
	this->targetObjectID = targetObjectID;
	this->result = result;
	this->correctedServerID = correctedServerID;
}

DistributedServerCommandRelayPacket::DistributedServerCommandRelayPacket(int commandType,
	int originServerID, int originSequence, int playerID, int clientSequence,
	const NCL::Interaction::CommandArgs& args) {
	type = BasicNetworkMessages::DistributedServerCommandRelay;
	size = sizeof(DistributedServerCommandRelayPacket) - sizeof(GamePacket);

	this->commandType = commandType;
	this->originServerID = originServerID;
	this->originSequence = originSequence;
	// Always 0 on send. A relay is never re-relayed; the receiver drops any packet
	// that arrives with hops already on it.
	this->hopCount = 0;
	this->playerID = playerID;
	this->clientSequence = clientSequence;
	this->args = args;
}

DistributedObjectSpawnedPacket::DistributedObjectSpawnedPacket(int objectID, int archetypeID,
	int ownerServerID, int spawnerPlayerID, const Vector3& position) {
	type = BasicNetworkMessages::DistributedObjectSpawned;
	size = sizeof(DistributedObjectSpawnedPacket) - sizeof(GamePacket);

	this->objectID = objectID;
	this->archetypeID = archetypeID;
	this->ownerServerID = ownerServerID;
	this->spawnerPlayerID = spawnerPlayerID;
	this->position = position;
}

DistributedObjectDespawnedPacket::DistributedObjectDespawnedPacket(int objectID, int reason,
	int destroyerPlayerID) {
	type = BasicNetworkMessages::DistributedObjectDespawned;
	size = sizeof(DistributedObjectDespawnedPacket) - sizeof(GamePacket);

	this->objectID = objectID;
	this->reason = reason;
	this->destroyerPlayerID = destroyerPlayerID;
}

HaloUpdatePacket::HaloUpdatePacket(int senderServerID, int senderTick) {
	type = BasicNetworkMessages::DistributedHaloUpdate;
	// Deliberately NOT sizeof(HaloUpdatePacket): an empty batch must not put 20
	// entries of uninitialised stack on the wire. TryAdd grows this.
	size = static_cast<short>(sizeof(HaloUpdatePacket) - sizeof(GamePacket)
		- sizeof(entries));

	this->senderServerID = senderServerID;
	this->senderTick = senderTick;
	this->entryCount = 0;
}

DistributedRepartitionPacket::DistributedRepartitionPacket(long long effectiveTick,
	int totalRegionCount) {
	type = BasicNetworkMessages::DistributedRepartition;
	// Sized to the regions actually used, like HaloUpdatePacket: a two-server
	// partition must not put a full page of uninitialised stack on the wire.
	size = static_cast<short>(sizeof(DistributedRepartitionPacket) - sizeof(GamePacket)
		- sizeof(regions));

	this->effectiveTick = effectiveTick;
	this->totalRegionCount = totalRegionCount;
	this->regionCount = 0;
}

DistributedServerRegistryPacket::DistributedServerRegistryPacket(int gameInstanceID,
	int totalServerCount) {
	type = BasicNetworkMessages::DistributedServerRegistry;
	size = static_cast<short>(sizeof(DistributedServerRegistryPacket) - sizeof(GamePacket)
		- sizeof(entries));

	this->gameInstanceID = gameInstanceID;
	this->totalServerCount = totalServerCount;
	this->entryCount = 0;
}

bool DistributedServerRegistryPacket::TryAddEntry(const ServerRegistryEntry& entry) {
	if (entryCount >= MAX_ENTRIES_PER_PAGE) {
		return false;
	}
	entries[entryCount] = entry;
	++entryCount;
	size = static_cast<short>(size + sizeof(ServerRegistryEntry));
	return true;
}

bool DistributedRepartitionPacket::TryAddRegion(const RegionBoundsWire& region) {
	if (regionCount >= MAX_REGIONS_PER_PAGE) {
		return false;
	}
	regions[regionCount] = region;
	++regionCount;
	size = static_cast<short>(size + sizeof(RegionBoundsWire));
	return true;
}

bool HaloUpdatePacket::TryAdd(const HaloObjectState& state) {
	if (entryCount >= MAX_ENTRIES) {
		return false;
	}
	entries[entryCount] = state;
	++entryCount;
	size = static_cast<short>(size + sizeof(HaloObjectState));
	return true;
}
#endif
