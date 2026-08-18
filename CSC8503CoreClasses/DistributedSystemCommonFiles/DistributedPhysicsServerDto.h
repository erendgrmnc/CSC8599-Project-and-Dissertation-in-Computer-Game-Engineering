#pragma once

#include <map>
#include <string>
#include <vector>

namespace NCL {
	struct GameBorder {
		double maxX = 0.f;
		double minX = 0.f;
		double minZ = 0.f;
		double maxZ = 0.f;

		GameBorder(double minX, double maxX, double minZ, double maxZ) {
			this->maxX = maxX;
			this->minX = minX;
			this->minZ = minZ;
			this->maxZ = maxZ;
		}
	};

	class GameInstance {
	public:
		GameInstance();
		// World bounds default to the legacy hardcoded -150..150 area so existing
		// callers keep their behaviour; the GUI launcher passes a configurable area.
		GameInstance(int id, int serverCount, int serverIDBuffer, int playerCountToStartServers = 1, int objectsToInstantiatePerPlayer = 1,
			double worldMinX = -150.0, double worldMaxX = 150.0, double worldMinZ = -150.0, double worldMaxZ = 150.0);

		int GetGameID();
		int GetServerCount();
		int AddPlayer(int peerID);
		int GetObjectsPerPlayer();
		const int GetPlayerCountToStartServers();

		bool IsServersReadyToStart();

		std::map<int, const std::string>& GetServerBorderStrMap();

		// Needed by whoever recomputes the partition at runtime. The manager owns
		// border calculation, so it is the only thing that should be reading these.
		const std::map<int, GameBorder*>& GetServerBorderMap() const {
			return mPhysicsServerBorderMap;
		}

		void GetWorldBounds(double& minX, double& maxX, double& minZ, double& maxZ) const {
			minX = mWorldMinX;
			maxX = mWorldMaxX;
			minZ = mWorldMinZ;
			maxZ = mWorldMaxZ;
		}
	protected:
		int mGameID;
		int mServerCount;
		int mPlayerCountToStartGame;
		int mObjectsToInstantiatePerPlayer;

		// Configurable world area the server regions are carved out of.
		double mWorldMinX = -150.0;
		double mWorldMaxX = 150.0;
		double mWorldMinZ = -150.0;
		double mWorldMaxZ = 150.0;

		int mPlayerIDBuffer;

		std::map<int, GameBorder*> mPhysicsServerBorderMap;
		std::map<int, const std::string> mPhysicsServerBorderStrMap;
		std::map<int, int> mClientPeerPlayerIDMap;

		void CalculatePhysicsServerBorders(int serverIDBuffer);
		void SetPhysicsServerBorderStrMap();
		void AddServerBorderDataToMap(std::pair<int, GameBorder*>& pair);
		

		std::string GetServerAreaString(int serverID);

		GameBorder& CalculateServerBorders(int serverNum);
	};

	class DistributedPhysicsServerData {
	public:

		DistributedPhysicsServerData(int serverID, int gameInstanceID, const std::string& ipAddress);
		~DistributedPhysicsServerData();

		bool GetIsServerStarted();
		void SetIsServerStarted(bool val);

		bool GetIsAllClientsConnectedToServer();
		void SetIsAllClientsConnectedToServer(bool isAllClientsConnectedToServer);

		const std::string& GetServerIPAddress();

		int GetDataSenderPort();
		void SetDataSenderPort(int dataSenderPort);

		int GetServerID();
		void SetServerID(int serverID);

		int GetGameInstanceID();
		void SetGameInstanceID(int gameInstanceID);
	protected:
		bool mIsServerStarted = false;
		bool mIsAllClientsConnectedToServer = false;

		std::string mIPAddress;
		int mDataSenderPort;
		int mServerID;
		int mGameInstanceID;
	};
}