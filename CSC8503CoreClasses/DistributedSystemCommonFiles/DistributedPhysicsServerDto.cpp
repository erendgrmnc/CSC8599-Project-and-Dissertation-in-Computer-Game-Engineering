#include "DistributedPhysicsServerDto.h"

#include <sstream>

namespace  {
	constexpr int GAME_AREA_MIN_X = -150;
	constexpr int GAME_AREA_MAX_X = 150;

	constexpr int GAME_AREA_MIN_Z = -150;
	constexpr int GAME_AREA_MAX_Z = 150;
}

NCL::GameInstance::GameInstance() = default;

NCL::GameInstance::GameInstance(int id, int serverCount, int serverIDBuffer, int playerCountToStartServers, int objectsPerPlayer) {
	this->mGameID = id;
	this->mServerCount = serverCount;
	mPlayerIDBuffer = -1;
	mPlayerCountToStartGame = playerCountToStartServers;
	mObjectsToInstantiatePerPlayer = objectsPerPlayer;

	CalculatePhysicsServerBorders(serverIDBuffer);
}

int NCL::GameInstance::GetGameID() {
	return mGameID;
}

int NCL::GameInstance::GetServerCount() {
	return mServerCount;
}

std::map<int, const std::string>& NCL::GameInstance::GetServerBorderStrMap() {
	return mPhysicsServerBorderStrMap;
}

void NCL::GameInstance::CalculatePhysicsServerBorders(int serverIDBuffer) {
	for (int i = serverIDBuffer; i < mServerCount; i++) {
		GameBorder& border = CalculateServerBorders(i);
		std::pair<int, GameBorder*> pair = std::make_pair(i, &border);
		AddServerBorderDataToMap(pair);
	}
	SetPhysicsServerBorderStrMap();
}

void NCL::GameInstance::SetPhysicsServerBorderStrMap() {
	std::cout << "-------------- SERVER BORDERS ---------------\n";
	for (const auto& pair : mPhysicsServerBorderMap) {
		const std::string& borderStr = GetServerAreaString(pair.first);
		std::pair<int, const std::string> strPair = std::make_pair(pair.first, borderStr);
		std::cout << "Server(" << pair.first << "): " << pair.second->minX << "," << pair.second->maxX << "|" << pair.second->minZ << ", " << pair.second->maxZ << "\n";
		mPhysicsServerBorderStrMap.insert(strPair);
	}
}

void NCL::GameInstance::AddServerBorderDataToMap(std::pair<int, GameBorder*>& pair) {
	mPhysicsServerBorderMap.insert(pair);
}

int NCL::GameInstance::AddPlayer(int peerID) {
	if (mClientPeerPlayerIDMap.contains(peerID)) {
		return mClientPeerPlayerIDMap[peerID];
	}

	int playerID = ++mPlayerIDBuffer;
	std::pair<int, int> playerPair = std::make_pair(peerID, playerID);
	mClientPeerPlayerIDMap.insert(playerPair);

	return playerID;
}

int NCL::GameInstance::GetObjectsPerPlayer() {
	return mObjectsToInstantiatePerPlayer;
}

const int NCL::GameInstance::GetPlayerCountToStartServers() {
	return mPlayerCountToStartGame;
}

bool NCL::GameInstance::IsServersReadyToStart() {
	return mClientPeerPlayerIDMap.size() == mPlayerCountToStartGame;
}

std::string NCL::GameInstance::GetServerAreaString(int serverID) {
	std::stringstream ss;
	GameBorder* borders = mPhysicsServerBorderMap[serverID];
	ss << borders->minX << "/" << borders->maxX << "|" << borders->minZ << "/" << borders->maxZ;
	return ss.str();
}

NCL::GameBorder& NCL::GameInstance::CalculateServerBorders(int serverNum) {
	if (serverNum < 0 || serverNum > mServerCount) {
		throw std::out_of_range("Server number out of range");
	}

	GameBorder* border = new GameBorder(0.f, 0.f, 0.f, 0.f);

	// Calculate the number of columns and rows based on the number of servers
	int numCols, numRows;
	if (mServerCount == 3) {
		// Special case for 3 servers
		numCols = 2;  // 2 columns
		numRows = 2;  // 2 rows, but we only use 1 row in the bottom row
	}
	else {
		double sqrt = std::sqrt(mServerCount);
		numCols = static_cast<int>(std::ceil(sqrt));
		numRows = static_cast<int>(std::ceil(static_cast<double>(mServerCount) / numCols));
	}

	// Calculate the width and height of each region
	int rectWidth = (GAME_AREA_MAX_X - GAME_AREA_MIN_X) / numCols;
	int rectHeight = (GAME_AREA_MAX_Z - GAME_AREA_MIN_Z) / numRows;

	// Determine which row and column this server is responsible for
	int row = serverNum / numCols;
	int col = serverNum % numCols;

	// Set the borders
	border->minX = GAME_AREA_MIN_X + col * rectWidth;
	border->minZ = GAME_AREA_MIN_Z + row * rectHeight;
	border->maxX = (col == numCols - 1) ? GAME_AREA_MAX_X : (border->minX + rectWidth);
	border->maxZ = (row == numRows - 1) ? GAME_AREA_MAX_Z : (border->minZ + rectHeight);

	// Adjustments for the special 3-server layout
	if (mServerCount == 3) {
		if (serverNum == 2) {
			// The bottom row server spans the full width
			border->minX = GAME_AREA_MIN_X;
			border->maxX = GAME_AREA_MAX_X;
		}
	}

	return *border;
}

NCL::DistributedPhysicsServerData::DistributedPhysicsServerData(int serverID, int gameInstanceID,
	const std::string& ipAddress) {
	mGameInstanceID = gameInstanceID;
	mServerID = serverID;
	mIPAddress = ipAddress;
}

NCL::DistributedPhysicsServerData::~DistributedPhysicsServerData() {
}

bool NCL::DistributedPhysicsServerData::GetIsServerStarted() {
	return mIsServerStarted;
}

void NCL::DistributedPhysicsServerData::SetIsServerStarted(bool val) {
	mIsServerStarted = val;
}

bool NCL::DistributedPhysicsServerData::GetIsAllClientsConnectedToServer() {
	return mIsAllClientsConnectedToServer;
}

void NCL::DistributedPhysicsServerData::SetIsAllClientsConnectedToServer(bool isAllClientsConnectedToServer) {

}

const std::string& NCL::DistributedPhysicsServerData::GetServerIPAddress() {
	return mIPAddress;
}

int NCL::DistributedPhysicsServerData::GetDataSenderPort() {
	return mDataSenderPort;
}

void NCL::DistributedPhysicsServerData::SetDataSenderPort(int dataSenderPort) {
	mDataSenderPort = dataSenderPort;
}

int NCL::DistributedPhysicsServerData::GetServerID() {
	return mServerID;
}

void NCL::DistributedPhysicsServerData::SetServerID(int serverID) {
	mServerID = serverID;
}

int NCL::DistributedPhysicsServerData::GetGameInstanceID() {
	return mGameInstanceID;
}

void NCL::DistributedPhysicsServerData::SetGameInstanceID(int gameInstanceID) {
	mGameInstanceID = gameInstanceID;
}
