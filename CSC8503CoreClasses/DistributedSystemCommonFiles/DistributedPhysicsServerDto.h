#pragma once
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
		GameInstance(int id, int serverCount, int serverIDBuffer, int playerCountToStartServers = 1);

		int GetGameID();
		int GetServerCount();
		int AddPlayer(int peerID);
		const int GetPlayerCountToStartServers();

		bool IsServersReadyToStart();

		std::map<int, const std::string>& GetServerBorderStrMap();
	protected:
		int mGameID;
		int mServerCount;
		int mPlayerCountToStartGame;

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