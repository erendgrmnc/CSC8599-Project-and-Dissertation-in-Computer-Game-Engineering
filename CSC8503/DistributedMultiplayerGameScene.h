#pragma once
#include "NetworkBase.h"
#include "NetworkObject.h"
#include "Vector4.h"

#include <map>
#include <vector>
#include <unordered_map>

namespace NCL {
	namespace Rendering { class Mesh; class Texture; class Shader; }
	namespace CSC8503 { class GameWorld; class GameObject; }
}

// One physics server the client is connected to, paired with its server ID so an
// incoming snapshot can be attributed to the server that actually sent it.
struct PhysicsServerLink {
	NCL::CSC8503::GameClient* client;
	int serverId;
};

// A server's slice of the world, in world XZ, plus the colour used for both its grid
// outline and the objects it owns.
struct ServerRegion {
	int serverId = -1;
	float minX = 0.f, maxX = 0.f, minZ = 0.f, maxZ = 0.f;
	NCL::Maths::Vector4 colour;
};

// Thin distributed-physics client: connects to the manager, is routed to a
// physics server, and receives world-state snapshots. Intentionally NOT part of
// the (removed) team-game Scene/LevelManager hierarchy - it is hosted by
// RunDistributedClient() in DistributedClientStart.cpp.
class DistributedMultiplayerGameScene : public PacketReceiver {
public:
	DistributedMultiplayerGameScene();
	~DistributedMultiplayerGameScene();

	bool ConnectClientToDistributedManager(char a, char b, char c, char d, int port);
	bool ConnectClientToDistributedGameServer(char a, char b, char c, char d, int port, const std::string& playerName, int serverId);

	// The game instance this client should join (the manager assigns id 1 to the
	// first/autostarted instance). Set before connecting.
	void SetGameInstanceId(int id) { mGameInstanceId = id; }

	bool IsGameStarted() const { return mIsGameStarted; }
	size_t GetReplicaCount() const { return mNetworkObjects.size(); }

	// Supplies the world + primitive render resources used to spawn one visible
	// replica per networked object. Set by the client host before connecting.
	// When the world is null (e.g. headless), snapshots are still received but no
	// replicas are built.
	void SetRenderResources(NCL::CSC8503::GameWorld* world, NCL::Rendering::Mesh* mesh,
		NCL::Rendering::Texture* albedo, NCL::Rendering::Texture* normal, NCL::Rendering::Shader* shader);

	// --- Interaction commands -------------------------------------------------
	// Returns the serverId a command must be sent to, or -1 if it cannot be
	// resolved. There is deliberately no broadcast fallback: a broadcast command
	// would be applied once per server.
	int ResolveCommandTarget(const NCL::Interaction::CommandArgs& args,
		const NCL::Interaction::CommandScope& scope) const;

	// Routes and sends. False means the command was rejected locally and never
	// left the machine.
	bool SendCommand(NCL::Interaction::CommandType type, NCL::Interaction::CommandArgs args);

	// Sends to a caller-chosen server instead of the resolved one. Exists so a test
	// can reproduce a stale owner table on demand: the real staleness window - the
	// moment between an object being handed over and the next snapshot arriving - is
	// only a few milliseconds wide and cannot be hit reliably from outside.
	// forcedServerId < 0 behaves exactly like SendCommand.
	bool SendCommandTo(NCL::Interaction::CommandType type, NCL::Interaction::CommandArgs args,
		int forcedServerId);

	// Server ids this client is connected to, in connection order.
	std::vector<int> GetConnectedServerIds() const;

	int GetCommandsSent() const { return mCommandsSent; }

	void UpdateGame(float dt);
	void UpdateDistributedManagerClient(float dt);
	void ReceivePacket(int type, GamePacket* payload, int source) override;
	void UpdatePhysicsClients(float dt);

	// --- Region visualisation -------------------------------------------------
	// The server regions discovered so far (one per server the client is routed to).
	const std::vector<ServerRegion>& GetServerRegions() const { return mServerRegions; }

	// Axis-aligned union of all regions received so far, for framing the camera.
	// Returns false until at least one region has arrived.
	bool GetWorldBounds(float& minX, float& maxX, float& minZ, float& maxZ) const;

	// Enables/disables the visualisation. When off, replicas revert to the default
	// colour; when on, they are tinted by their owning server. Recolours existing
	// replicas immediately.
	void SetOverlayEnabled(bool enabled);
	bool IsOverlayEnabled() const { return mOverlayEnabled; }

	// The fixed per-server palette, shared by grid outlines and object tints.
	static NCL::Maths::Vector4 ColourForServer(int serverId);
protected:
	bool mIsGameStarted = false;
	int mGameInstanceId = 1;

	int mClientSideLastFullID;
	int mServerSideLastFullID;

	int mNetworkObjectCache = 10;

	NCL::CSC8503::GameClient* mDistributedManagerClient = nullptr;
	std::vector<PhysicsServerLink> mDistributedPhysicsClients;

	// Regions keyed implicitly by serverId; the server whose snapshot is currently
	// being processed (set around each client's UpdateClient pump, else -1).
	std::vector<ServerRegion> mServerRegions;
	int mActiveServerId = -1;
	bool mOverlayEnabled = true;

	// Client-side world + render resources for the visible replicas.
	NCL::CSC8503::GameWorld* mWorld = nullptr;
	NCL::Rendering::Mesh* mObjMesh = nullptr;
	NCL::Rendering::Texture* mObjAlbedo = nullptr;
	NCL::Rendering::Texture* mObjNormal = nullptr;
	NCL::Rendering::Shader* mObjShader = nullptr;
	std::vector<NCL::CSC8503::NetworkObject*> mNetworkObjects;

	// objectID -> the server that last sent a snapshot for it, so replicas can be
	// recoloured when the overlay is toggled without waiting for the next snapshot.
	std::unordered_map<int, int> mObjectOwner;

	void HandleFullPacket(NCL::CSC8503::FullPacket* packet);

	// Replicas created with no GameWorld to own them (headless). Held so they are
	// not leaked and can be torn down with the scene.
	std::vector<NCL::CSC8503::GameObject*> mHeadlessReplicas;

	// Snapshot acknowledgement, keyed by physics server ID. Sent once per pump rather
	// than per packet: a full snapshot is one packet per object.
	void SendSnapshotAcks();
	// One global sequence across every server link, not one per link: (playerID,
	// sequence) must be unique whichever server ends up applying the command.
	int mNextCommandSequence = 1;
	int mCommandsSent = 0;
	std::map<int, int> mAckResultCounts;   // CommandResult -> count, for the I4 invariant

	std::map<int, int> mLastFullStateIdPerServer;
	std::map<int, int> mLastAckedStateIdPerServer;
	void HandleDeltaPacket(NCL::CSC8503::DeltaPacket* packet);
	NCL::CSC8503::NetworkObject* FindNetworkObject(int objectID);
	NCL::CSC8503::NetworkObject* SpawnReplica(int objectID);

	// Tints a replica by mActiveServerId (or default colour when the overlay is off).
	void ApplyOwnerColour(NCL::CSC8503::NetworkObject* netObj, int serverId);

	void SendGameClientConnectedPacket(int gameInstanceID);
	void HandleOnConnectToDistributedPhysicsServerPacketReceived(NCL::CSC8503::DistributedClientConnectToPhysicsServerPacket* packet);
	void HandleGameStartPacketReceived(NCL::CSC8503::GameStartStatePacket* packet);
	std::vector<char> IpToCharArray(const std::string& ipAddress);
};
