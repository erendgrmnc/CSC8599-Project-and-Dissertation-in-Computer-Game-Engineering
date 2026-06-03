#pragma once
#include "NetworkBase.h"
#include "NetworkObject.h"

// Thin distributed-physics client: connects to the manager, is routed to a
// physics server, and receives world-state snapshots. Intentionally NOT part of
// the (removed) team-game Scene/LevelManager hierarchy - it is hosted by
// RunDistributedClient() in DistributedClientStart.cpp.
class DistributedMultiplayerGameScene : public PacketReceiver {
public:
	DistributedMultiplayerGameScene();
	~DistributedMultiplayerGameScene();

	bool ConnectClientToDistributedManager(char a, char b, char c, char d, int port);
	bool ConnectClientToDistributedGameServer(char a, char b, char c, char d, int port, const std::string& playerName);

	// The game instance this client should join (the manager assigns id 1 to the
	// first/autostarted instance). Set before connecting.
	void SetGameInstanceId(int id) { mGameInstanceId = id; }

	void UpdateGame(float dt);
	void UpdateDistributedManagerClient(float dt);
	void ReceivePacket(int type, GamePacket* payload, int source) override;
	void UpdatePhysicsClients(float dt) const;
protected:
	bool mIsGameStarted = false;
	int mGameInstanceId = 1;

	int mClientSideLastFullID;
	int mServerSideLastFullID;

	int mNetworkObjectCache = 10;

	NCL::CSC8503::GameClient* mDistributedManagerClient = nullptr;
	std::vector<NCL::CSC8503::GameClient*> mDistributedPhysicsClients;

	void SendGameClientConnectedPacket(int gameInstanceID);
	void HandleOnConnectToDistributedPhysicsServerPacketReceived(NCL::CSC8503::DistributedClientConnectToPhysicsServerPacket* packet);
	void HandleGameStartPacketReceived(NCL::CSC8503::GameStartStatePacket* packet);
	std::vector<char> IpToCharArray(const std::string& ipAddress);
};
