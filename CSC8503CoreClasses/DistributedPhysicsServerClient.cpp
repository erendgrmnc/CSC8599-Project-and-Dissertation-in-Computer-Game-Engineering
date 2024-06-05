#ifdef USEGL
#include "./DistributedPhysicsServerClient.h"

#endif
#include "enet/enet.h"

NCL::Networking::DistributedPhysicsServerClient::DistributedPhysicsServerClient() : GameClient() {

}

NCL::Networking::DistributedPhysicsServerClient::~DistributedPhysicsServerClient() {
}


bool NCL::Networking::DistributedPhysicsServerClient::UpdatePhysicsServer() {
	return DistributedPhysicsServerClient::UpdateClient();
}

bool NCL::Networking::DistributedPhysicsServerClient::UpdateClient() {
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
			std::cout << "Connected to Distributed Manager!" << std::endl;
			TriggerOnAllClientsAreConnectedEvents();
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

void NCL::Networking::DistributedPhysicsServerClient::DisconnectFromDistributedServerManager() {
	GameClient::Disconnect();
}

void NCL::Networking::DistributedPhysicsServerClient::RegisterOnConnectedToDistributedManagerEvent(
	const std::function<void()>& callback) {
	mOnAllClientsAreConnected.push_back(callback);
}

void NCL::Networking::DistributedPhysicsServerClient::TriggerOnAllClientsAreConnectedEvents() const {
	for (const auto& callback : mOnAllClientsAreConnected) {
		callback();
	}
}
