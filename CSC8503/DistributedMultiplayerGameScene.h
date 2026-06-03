#pragma once
#include "NetworkBase.h"
#include "NetworkObject.h"

namespace NCL {
	namespace Rendering { class Mesh; class Texture; class Shader; }
	namespace CSC8503 { class GameWorld; }
}

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

	bool IsGameStarted() const { return mIsGameStarted; }

	// Supplies the world + primitive render resources used to spawn one visible
	// replica per networked object. Set by the client host before connecting.
	// When the world is null (e.g. headless), snapshots are still received but no
	// replicas are built.
	void SetRenderResources(NCL::CSC8503::GameWorld* world, NCL::Rendering::Mesh* mesh,
		NCL::Rendering::Texture* albedo, NCL::Rendering::Texture* normal, NCL::Rendering::Shader* shader);

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

	// Client-side world + render resources for the visible replicas.
	NCL::CSC8503::GameWorld* mWorld = nullptr;
	NCL::Rendering::Mesh* mObjMesh = nullptr;
	NCL::Rendering::Texture* mObjAlbedo = nullptr;
	NCL::Rendering::Texture* mObjNormal = nullptr;
	NCL::Rendering::Shader* mObjShader = nullptr;
	std::vector<NCL::CSC8503::NetworkObject*> mNetworkObjects;

	void HandleFullPacket(NCL::CSC8503::FullPacket* packet);
	void HandleDeltaPacket(NCL::CSC8503::DeltaPacket* packet);
	NCL::CSC8503::NetworkObject* FindNetworkObject(int objectID);
	NCL::CSC8503::NetworkObject* SpawnReplica(int objectID);

	void SendGameClientConnectedPacket(int gameInstanceID);
	void HandleOnConnectToDistributedPhysicsServerPacketReceived(NCL::CSC8503::DistributedClientConnectToPhysicsServerPacket* packet);
	void HandleGameStartPacketReceived(NCL::CSC8503::GameStartStatePacket* packet);
	std::vector<char> IpToCharArray(const std::string& ipAddress);
};
