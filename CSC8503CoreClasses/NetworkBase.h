#ifdef USEGL
#pragma once
//#include "./enet/enet.h"
struct _ENetHost;
struct _ENetPeer;
struct _ENetEvent;

enum BasicNetworkMessages {
	None,
	Hello,
	Message,
	String_Message,
	Delta_State,	//1 byte per channel since the last state
	Full_State,		//Full transform etc
	Received_State, //received from a client, informs that its received packet n
	Player_Connected,
	Player_Disconnected,
	Shutdown,
	VariableUpdate,
	SyncPlayers,
	GameStartState,
	GameEndState,
	ClientPlayerInputState,
	ClientSyncItemSlotUsage,
	ClientSyncItemSlot,
	ClientSyncBuffs,
	ClientSyncLocalActiveCause,
	ClientSyncLocalSusChange,
	ClientSyncLocationActiveCause,
	ClientSyncLocationSusChange,
	SyncInteractable,
	ClientSyncGlobalSusChange,
	SyncObjectState,
	ClientInit,
	SyncPlayerIdNameMap,
	SyncAnnouncements,
	GuardSpotSound,
	//Distributed System Packet Types
	DistributedClientConnectedToManager,
	DistributedPhysicsClientConnectedToManager,
	DistributedPhysicsServerAllClientsAreConnected,
	DistributedClientConnectToPhysicsServer,
	DistributedClientsGameServersAreReady,
	StartDistributedPhysicsServer,
	StartSimulatingObjectInServer,
	StartSimulatingObjectInServerReceived,
	RunDistributedPhysicsServerInstance,
	DistributedClientGetGameInstanceData,
	PhysicsServerMiddlewareConnected,
	PhysicsServerMiddlewareData,
	AddTestObjectsToTheWorld,
	// Client -> game server: acknowledges the newest full snapshot the client has
	// applied. Deltas are encoded relative to a full state every client is known to
	// hold, so without this the server has no baseline and every delta is discarded.
	// APPEND ONLY - the four roles are built and deployed separately, so inserting
	// anywhere above silently renumbers the wire protocol.
	DistributedClientSnapshotAck,
	// Dynamic interaction types. One packet shape carries every interaction; the
	// payload is interpreted by the IInteractionCommand registered for its
	// commandType, so adding a new interaction adds NO message types.
	// APPEND ONLY, for the same reason as above.
	DistributedClientCommand,        // Client      -> Game Server
	DistributedCommandAck,           // Game Server -> Client
	DistributedServerCommandRelay,   // Game Server -> Game Server
	DistributedObjectSpawned,        // Game Server -> peers + clients
	DistributedObjectDespawned,      // Game Server -> peers + clients
	// Owning game server -> the neighbouring servers whose regions its objects are
	// close to. Carries a batch of read-only object states so that objects either
	// side of a region border can collide. APPEND ONLY, as above.
	DistributedHaloUpdate,           // Game Server -> Game Server
	// Manager -> every game server and client: the partition changes to these
	// borders at an absolute tick. APPEND ONLY, as above.
	DistributedRepartition,          // Manager -> Game Servers + Clients
	// Manager -> game servers: one PAGE of the server registry. Replaces the fixed
	// 20-entry arrays that used to ride inside the start packet. APPEND ONLY.
	DistributedServerRegistry        // Manager -> Game Servers
};

enum DistributedSystemClientType {
	DistributedGameClient,
	DistributedPhysicsClient
};

enum DistributedSystemServerType {
	
};

// Copies a std::string into a fixed-size packet field, always null-terminating and
// truncating rather than running off the end.
//
// Packets are memcpy'd onto the wire (GameClient::SendPacket), so a std::string
// member only ever "worked" because short-string optimisation kept the bytes inline
// AND both ends were the same MSVC x64 binary. A longer value, a different STL, or a
// heap-allocated string would have put a pointer on the wire.
template<size_t N>
inline void CopyToPacketField(char (&dst)[N], const std::string& src) {
	const size_t count = (src.size() < N - 1) ? src.size() : (N - 1);
	if (count > 0) {
		memcpy(dst, src.data(), count);
	}
	dst[count] = 0;
}

// Long enough for "255.255.255.255" plus a terminator.
constexpr size_t PACKET_IP_LENGTH = 16;

struct GamePacket {
	short size;
	short type;

	GamePacket() {
		type		= BasicNetworkMessages::None;
		size		= 0;
	}

	GamePacket(short type) : GamePacket() {
		this->type	= type;
	}

	int GetTotalSize() {
		return sizeof(GamePacket) + size;
	}
};

struct StringPacket : public GamePacket {
	char stringData[256];

	StringPacket(const std::string& message) {
		type = BasicNetworkMessages::String_Message;
		size = (short)message.length();

		memcpy(stringData, message.data(), size);
	}

	std::string GetStringFromData() {
		std::string realString(stringData);
		realString.resize(size);
		return realString;
	}
};

struct VariablePacket : public GamePacket {
	int itemsLeft;

	VariablePacket(const int& itemsLeftUpdate) {
		type = BasicNetworkMessages::VariableUpdate;
		//size = (short)message.length();

		itemsLeft = itemsLeftUpdate;
	}
};

class PacketReceiver {
public:
	virtual void ReceivePacket(int type, GamePacket* payload, int source = -1) = 0;
};

class NetworkBase	{
public:
	static void Initialise();
	static void Destroy();

	static int GetDefaultPort() {
		return 1234;
	}

	void RegisterPacketHandler(int msgID, PacketReceiver* receiver) {
		packetHandlers.insert(std::make_pair(msgID, receiver));
	}

	void ClearPacketHandlers();
protected:
	NetworkBase();
	~NetworkBase();

	bool ProcessPacket(GamePacket* p, int peerID = -1);

	typedef std::multimap<int, PacketReceiver*>::const_iterator PacketHandlerIterator;

	bool GetPacketHandlers(int msgID, PacketHandlerIterator& first, PacketHandlerIterator& last) const {
		auto range = packetHandlers.equal_range(msgID);

		if (range.first == packetHandlers.end()) {
			return false; //no handlers for this message type!
		}
		first	= range.first;
		last	= range.second;
		return true;
	}

	_ENetHost* netHandle;

	std::multimap<int, PacketReceiver*> packetHandlers;
};
#endif