#ifdef USEGL

#include "GameClient.h"

#include "NetworkObject.h"
#include "./enet/enet.h"
using namespace NCL;
using namespace CSC8503;

GameClient::GameClient()	{
	netHandle = enet_host_create(nullptr, 1, 1, 0, 0);
	mTimerSinceLastPacket = 0.0f;
	mPeerId = -1;
	mIsConnected = false;
	mPlayerInputs = new PlayerInputs();
	mClientSideLastFullID = -1;
}

GameClient::~GameClient()	{
	enet_host_destroy(netHandle);
	delete mPlayerInputs;
}

int GameClient::GetPeerID() const {
	return mPeerId;
}

const int GameClient::GetClientLastFullID() const {
	return mClientSideLastFullID;
}

void GameClient::SetClientLastFullID(const int clientLastFullID) {
	mClientSideLastFullID = clientLastFullID;
}

bool GameClient::Connect(uint8_t a, uint8_t b, uint8_t c, uint8_t d, int portNum, const std::string& playerName) {
	ENetAddress address;
	address.port = portNum;
	address.host = (d << 24) | (c << 16) | (b << 8) | (a);

	mNetPeer = enet_host_connect(netHandle, &address, 2, 0);
	mPlayerName = playerName;

	// returm false if net peer is null
	return mNetPeer != nullptr;
}

bool GameClient::UpdateClient() {
	// if there is no net handle we cannot handle packets
	if (netHandle == nullptr)
		return false;

	mTimerSinceLastPacket++;

	// handle incoming packets
	ENetEvent event;
	while (enet_host_service(netHandle, &event, 0) > 0) {
		if (event.type == ENET_EVENT_TYPE_CONNECT) {
			//erendgrmnc: I remember +1 is needed because when counting server as a player, outgoing peer Id is not increasing.
			mPeerId = mNetPeer->outgoingPeerID + 1;
			mIsConnected = true;
			mLinkLost = false;
			std::cout << "Connected to server!" << std::endl;

			for (const auto& callback : mOnClientConnectedToServer) {
				callback();
			}

			//TODO(eren.degirmenci): send player init packet.
			SendClientInitPacket();
		}
		else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
			// Previously not handled AT ALL in the live loop - only in Disconnect(),
			// the deliberate-shutdown path. An ENet peer that drops mid-run therefore
			// left mIsConnected true and mNetPeer non-null, so:
			//
			//   * GetIsConnected() and anything built on it reported a healthy link,
			//   * every enet_peer_send refused the packet, because ENet will not send
			//     on a peer that is not CONNECTED, and
			//   * nothing ever reconnected, because nothing knew there was anything
			//     to reconnect.
			//
			// On a two-server injection run that made one server's handoffs fail for
			// the rest of the run while it still claimed to have the link - observed
			// on 8 of 8 runs, roughly 13 s in.
			//
			// event.data is whatever the peer passed to enet_peer_disconnect; 0 is
			// what a timeout reports, since no peer chose it.
			mIsConnected = false;
			mLinkLost = true;
			// mNetPeer is deliberately LEFT set. ENet has already reset the peer, so
			// enet_peer_send refuses on it safely, and ReclaimDroppedPeers destroys
			// this client - and with it the host that owns the peer - so the pointer
			// cannot outlive its target or be reused behind our back. Nulling it here
			// instead crashed the server outright: SendPacket dereferences mNetPeer
			// with no guard, so the first unreliable send after a dropped link took
			// the process down.
			std::cout << "Client: link to server lost (reason "
				<< event.data << ", 0 = timeout)\n";
			for (const auto& callback : mOnClientDisconnectedFromServer) {
				callback();
			}
		}
		else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
			//std::cout << "Client Packet recieved..." << std::endl;
			GamePacket* packet = (GamePacket*)event.packet->data;
			ProcessPacket(packet);
			mTimerSinceLastPacket = 0.0f;
		}
		// once packet data is handled we can destroy packet and go to next
		enet_packet_destroy(event.packet);
	}
	// return false if client is no longer receiving packets
	if (mTimerSinceLastPacket > 20.0f) {
		return false;
	}
	return true;
}

void GameClient::WriteAndSendClientInputPacket(int playerID){

	ClientPlayerInputPacket packet(mClientSideLastFullID, playerID, *mPlayerInputs);
	this->SendPacket(packet);
}

void GameClient::SendPacket(GamePacket&  payload) {
	// Guarded, unlike the original. Every other path through this class checks
	// mNetPeer; this one dereferenced it unconditionally, so any caller reaching it
	// before a connection existed - or after one was torn down - crashed the process.
	if (mNetPeer == nullptr) {
		return;
	}
	ENetPacket* dataPacket = enet_packet_create(&payload, payload.GetTotalSize(), 0);
	if (dataPacket == nullptr) {
		return;
	}
	// Unreliable, so a refusal is not worth reporting - but ENet does NOT take
	// ownership of a packet it refused, so it still has to be destroyed here.
	if (enet_peer_send(mNetPeer, 0, dataPacket) < 0) {
		enet_packet_destroy(dataPacket);
	}
}

// Returns whether ENet ACCEPTED the packet.
//
// It used to return void and discard enet_peer_send's result, which made every caller
// unable to tell a queued packet from a refused one. That is fine for a snapshot and
// fatal for a handoff: SendPacketToServer reported success regardless, so the sender
// released the object even when the packet had been refused, and the object was lost.
// It showed up when a border move tried to migrate thousands of objects at once and
// filled the peer's outgoing queue - 6,026 transfers sent, 516 accounted for.
//
// On failure ENet does NOT take ownership of the packet, so it has to be destroyed
// here or every refused send leaks.
bool GameClient::SendReliablePacket(GamePacket& payload) const {
	if (mNetPeer == nullptr) {
		return false;
	}
	ENetPacket* dataPacket = enet_packet_create(&payload, payload.GetTotalSize(), ENET_PACKET_FLAG_RELIABLE);
	if (dataPacket == nullptr) {
		return false;
	}
	if (enet_peer_send(mNetPeer, 0, dataPacket) < 0) {
		enet_packet_destroy(dataPacket);
		return false;
	}
	return true;
}

void GameClient::Disconnect() {
	if (mNetPeer != nullptr) {
		// disconnect_later, NOT disconnect. enet_peer_disconnect calls
		// enet_peer_reset_queues, which throws away every outgoing reliable command
		// that has not been sent AND every sent one still awaiting an acknowledgement.
		// Reliable therefore does not mean reliable across a shutdown: whatever this
		// client sent in its last few milliseconds is silently discarded. That showed
		// up as an I4 shortfall of one or two commands that appeared and disappeared
		// with timing. disconnect_later holds the peer open until the queues drain and
		// only then sends the disconnect.
		enet_peer_disconnect_later(mNetPeer, 0);
		enet_host_flush(netHandle);

		// Service in a loop with a deadline, not a single blocking call: the queues
		// only drain while the host is serviced, and the first event to arrive is
		// usually an inbound snapshot rather than the disconnect. The original
		// single-call form reported failure on any other event, which is why a clean
		// shutdown still printed "Failed to disconnect".
		constexpr enet_uint32 DISCONNECT_TIMEOUT_MS = 3000;
		constexpr enet_uint32 SERVICE_SLICE_MS = 50;
		bool disconnected = false;
		ENetEvent event;
		for (enet_uint32 waited = 0; waited < DISCONNECT_TIMEOUT_MS && !disconnected;
			waited += SERVICE_SLICE_MS) {
			while (enet_host_service(netHandle, &event, SERVICE_SLICE_MS) > 0) {
				if (event.type == ENET_EVENT_TYPE_RECEIVE) {
					// Inbound traffic during shutdown is not interesting, but it must
					// still be destroyed or the host leaks it.
					enet_packet_destroy(event.packet);
				}
				else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
					disconnected = true;
					break;
				}
			}
		}

		if (disconnected) {
			std::cout << "Disconnected from the server." << std::endl;
		}
		else {
			// Timed out with packets still queued. Force it, and say so - this is the
			// case where commands CAN still be lost.
			std::cerr << "Disconnect timed out; forcing. Queued packets may be lost."
				<< std::endl;
			enet_peer_reset(mNetPeer);
		}

		// Reset the peer to nullptr after disconnecting
		mNetPeer = nullptr;
	}
	mIsConnected = false;
}

bool GameClient::GetIsConnected() const {
	return mIsConnected;
}

void GameClient::SendClientInitPacket() {
	ClientInitPacket packet(mPlayerName);
	SendPacket(packet);
}

void GameClient::WriteAndSendAnnouncementSyncPacket(int annType, float time, int playerNo) {
	AnnouncementSyncPacket packet(annType, time, playerNo);
	this->SendPacket(packet);
}

void GameClient::WriteAndSendInteractablePacket(int networkObjectId, bool isOpen, int interactableItemType) {
	SyncInteractablePacket packet(networkObjectId, isOpen, interactableItemType);
	this->SendPacket(packet);
}
void GameClient::WriteAndSendInventoryPacket(int playerNo, int invSlot, int inItem, int usageCount){
	ClientSyncItemSlotPacket packet(playerNo, invSlot, inItem, usageCount);
	this->SendPacket(packet);
}
void GameClient::WriteAndSendSyncLocationSusChangePacket(int cantorPairedLocation, int changedValue) {
	ClientSyncLocationSusChangePacket packet(cantorPairedLocation, changedValue);
	this->SendPacket(packet);
}

void GameClient::SetPlayerInputs(PlayerInputs& playerInputs) {
	mPlayerInputs = &playerInputs;
}

void GameClient::SetPlayerInputs(bool movementButtons[4]) {
	mPlayerInputs->movementButtons[0] = movementButtons[0];
	mPlayerInputs->movementButtons[1] = movementButtons[1];
	mPlayerInputs->movementButtons[2] = movementButtons[2];
	mPlayerInputs->movementButtons[3] = movementButtons[3];
}

void GameClient::AddOnClientConnected(const std::function<void()>& callback) {
	mOnClientConnectedToServer.push_back(callback);
}

std::string GameClient::GetIPAddress() {
	if (!netHandle) {
		return ""; // Or handle the error appropriately
	}

	char ipAddress[15];
	enet_address_get_host_ip(&netHandle->address, ipAddress, 15);
	return std::string(ipAddress);
}
#endif
