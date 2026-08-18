#ifdef USEGL
#include "GameServer.h"
#include "GameWorld.h"
#include "./enet/enet.h"
using namespace NCL;
using namespace CSC8503;

GameServer::GameServer(int onPort, int maxClients, bool isStartingServer) {
	mPort = onPort;
	mClientMax = maxClients;
	mClientCount = 0;
	netHandle = nullptr;
	// Was a fixed new int[20] while the loop below (and AddPeer) run to mClientMax,
	// which SetMaxClients can raise freely - a heap overflow for any configuration
	// with more than 20 total peers.
	mPeers = new int[mClientMax];
	for (int i = 0; i < mClientMax; ++i) {
		mPeers[i] = -1;
	}

	if (isStartingServer) {
		Initialise();
	}
}

GameServer::~GameServer() {
	Shutdown();
	delete[] mPeers;
	mPeers = nullptr;
}

void GameServer::Shutdown() {
	if (netHandle == nullptr) {
		return;
	}

	SendGlobalPacket(BasicNetworkMessages::Shutdown);

	// enet_host_destroy does not flush. Without the drain below it discarded every
	// queued outgoing packet - including the Shutdown notification queued on the line
	// above, which therefore never arrived, and any handoff still in flight, which
	// lost the object outright. Same fault as GameClient::Disconnect had.
	enet_host_flush(netHandle);

	for (const auto& handle : mPeerHandles) {
		if (handle.second != nullptr) {
			// disconnect_later, so the peer stays open until its queues drain rather
			// than resetting them the way enet_peer_disconnect would.
			enet_peer_disconnect_later(handle.second, 0);
		}
	}

	// Bounded: a peer that has already gone away must not stall the exit, and a
	// server exiting on --run-ticks is being timed.
	constexpr enet_uint32 DRAIN_TIMEOUT_MS = 1000;
	constexpr enet_uint32 SERVICE_SLICE_MS = 25;
	size_t remaining = mPeerHandles.size();
	ENetEvent event;
	for (enet_uint32 waited = 0; waited < DRAIN_TIMEOUT_MS && remaining > 0;
		waited += SERVICE_SLICE_MS) {
		while (enet_host_service(netHandle, &event, SERVICE_SLICE_MS) > 0) {
			if (event.type == ENET_EVENT_TYPE_RECEIVE) {
				enet_packet_destroy(event.packet);
			}
			else if (event.type == ENET_EVENT_TYPE_DISCONNECT && remaining > 0) {
				--remaining;
			}
		}
	}

	mPeerHandles.clear();
	enet_host_destroy(netHandle);
	netHandle = nullptr;
}

bool GameServer::Initialise() {
	// create game server
	ENetAddress address;
	address.host = ENET_HOST_ANY;
	address.port = mPort;

	netHandle = enet_host_create(&address, mClientMax, 1, 0, 0);

	// if server is not set up then diplay error message and return false
	if (!netHandle) {
		std::cout << __FUNCTION__ << "failed to create network handle!" << std::endl;
		return false;
	}

	char ipString[16];
	enet_address_get_host_ip(&netHandle->address, ipString, sizeof(ipString));
	std::cout << "Local IP Address: " << ipString << std::endl;

	return true;
}

bool GameServer::SendGlobalPacket(int msgID) {
	GamePacket packet;
	packet.type = msgID;
	return SendGlobalPacket(packet);
}

bool GameServer::SendGlobalReliablePacket(GamePacket& packet) {
	ENetPacket* dataPacket = enet_packet_create(&packet, packet.GetTotalSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_host_broadcast(netHandle, 0, dataPacket);
	return true;
}

bool GameServer::SendGlobalPacket(GamePacket& packet) {
	// define and send packet
	ENetPacket* dataPacket = enet_packet_create(&packet, packet.GetTotalSize(), 0);
	enet_host_broadcast(netHandle, 0, dataPacket);
	return true;
}

bool GameServer::SendVariableUpdatePacket(VariablePacket& packet) {
	ENetPacket* dataPacket = enet_packet_create(&packet, packet.GetTotalSize(), 0);
	enet_host_broadcast(netHandle, 0, dataPacket);
	return true;
}

bool GameServer::GetPeer(int peerNumber, int& peerId) const
{
	if (peerNumber >= mClientMax)
		return false;
	if (mPeers[peerNumber] == -1) {
		return false;
	}
	peerId = mPeers[peerNumber];
	return true;
}

std::string GameServer::GetIpAddress() const {
	return ipAddress;
}

void GameServer::UpdateServer() {
	if (!netHandle) { return; }

	ENetEvent event;
	while (enet_host_service(netHandle, &event, 0) > 0) {
		int type = event.type;
		ENetPeer* p = event.peer;
		int peer = p->incomingPeerID;

		if (type == ENetEventType::ENET_EVENT_TYPE_CONNECT) {
			std::cout << "Server: New client has connected" << std::endl;
			// Retain the handle before AddPeer runs: a subclass override may want to
			// send this peer a directed packet (the late-join manifest does exactly
			// that), and without the handle there is nothing to send to.
			mPeerHandles[peer + 1] = p;
			AddPeer(peer + 1);
		}
		else if (type == ENetEventType::ENET_EVENT_TYPE_DISCONNECT) {
			std::cout << "Server: Client has disconnected" << std::endl;
			// Was a hardcoded 3, so peers in any slot past index 2 were never
			// released and mClientCount never fell - the slot leaked for the rest of
			// the run and eventually the table filled up.
			for (int i = 0; i < mClientMax; ++i) {
				if (mPeers[i] == peer + 1) {
					mPeers[i] = -1;
					mClientCount--;
				}
			}
			mPeerHandles.erase(peer + 1);

		}
		else if (type == ENetEventType::ENET_EVENT_TYPE_RECEIVE) {
			//std::cout << "Server: Has recieved packet" << std::endl;
			GamePacket* packet = (GamePacket*)event.packet->data;
			ProcessPacket(packet, peer);
		}
		enet_packet_destroy(event.packet);
	}
}

void GameServer::SetMaxClients(int maxClients) {
	if (maxClients == mClientMax) {
		return;
	}
	// The peer table must grow with the bound: every loop over mPeers runs to
	// mClientMax, so raising the count without reallocating overflows the buffer.
	int* resized = new int[maxClients];
	for (int i = 0; i < maxClients; ++i) {
		resized[i] = (i < mClientMax) ? mPeers[i] : -1;
	}
	delete[] mPeers;
	mPeers = resized;
	mClientMax = maxClients;

	// Recount from the retained table. Shrinking the bound drops any peer sitting in
	// a slot past the new end, and leaving mClientCount at its old value would make
	// the count permanently disagree with the table - so a "have all peers arrived?"
	// test could never come true again.
	mClientCount = 0;
	for (int i = 0; i < mClientMax; ++i) {
		if (mPeers[i] != -1) {
			++mClientCount;
		}
	}
}

void GameServer::SetGameWorld(GameWorld& g) {
	mGameWorld = &g;
}

bool GameServer::SendPacketToPeer(int peerNumber, GamePacket& packet) {
	const auto entry = mPeerHandles.find(peerNumber);
	if (entry == mPeerHandles.end() || entry->second == nullptr) {
		return false;
	}

	ENetPacket* dataPacket = enet_packet_create(&packet, packet.GetTotalSize(),
		ENET_PACKET_FLAG_RELIABLE);
	return enet_peer_send(entry->second, 0, dataPacket) == 0;
}

void GameServer::AddPeer(int peerNumber) {
	int emptyIndex = mClientMax;
	for (int i = 0; i < mClientMax; i++) {
		if (mPeers[i] == peerNumber) {
			return;
		}
		if (mPeers[i] == -1) {
			emptyIndex = std::min(i, emptyIndex);
		}
	}
	if (emptyIndex < mClientMax) {
		mPeers[emptyIndex] = peerNumber;
		mClientCount++;
	}
}
#endif