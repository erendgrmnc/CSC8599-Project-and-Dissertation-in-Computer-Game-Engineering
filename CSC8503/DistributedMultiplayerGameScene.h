#pragma once
#include "NetworkedGame.h"
#include "NetworkObject.h"

class DistributedMultiplayerGameScene : public NCL::CSC8503::NetworkedGame {
public:
	DistributedMultiplayerGameScene();
	~DistributedMultiplayerGameScene();

	bool ConnectClientToDistributedManager(char a, char b, char c, char d, int port);
	bool ConnectClientToDistributedGameServer(char a, char b, char c, char d, int port, const std::string& playerName);

	void UpdateGame(float dt) override;
	void UpdateDistributedManagerClient(float dt);
	void ReceivePacket(int type, GamePacket* payload, int source) override;
	void UpdatePhysicsClients(float dt) const;
protected:
	int mClientSideLastFullID;
	int mServerSideLastFullID;

	int mNetworkObjectCache = 10;

	NCL::CSC8503::GameClient* mDistributedManagerClient = nullptr;
	std::vector<NCL::CSC8503::GameClient*> mDistributedPhysicsClients;

	void SetItemsLeftToZero();

	void HandleOnConnectToDistributedPhysicsServerPacketReceived(DistributedClientConnectToPhysicsServerPacket* packet);
	std::vector<char> IpToCharArray(const std::string& ipAddress);
};
