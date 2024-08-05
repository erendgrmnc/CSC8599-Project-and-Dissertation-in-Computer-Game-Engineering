#include "TestObject.h"

#include <vector>

#include "NetworkObject.h"
#include "StateTransition.h"
#include "StateMachine.h"
#include "State.h"
#include "PhysicsObject.h"

using namespace NCL;
using namespace CSC8503;

NCL::CSC8503::TestObject::TestObject(DistributedGameServer::PhyscisServerBorderData& data, int playerID) {
	mBorderData = data;
	this->mPlayerID = playerID;
}


TestObject::~TestObject() = default;


void TestObject::Update(float dt) {
	if (mPlayerInputs[0]) {
		MoveLeft(dt);
	}
	if (mPlayerInputs[1]) {
		MoveRight(dt);
	}
}

void TestObject::ReceiveClientInputs(DistributedClientPacket* clientPacket) {
	if (clientPacket[1].movementButtons[0] == true) {
		std::cout << "Sol\n";
	}

	if (clientPacket[1].movementButtons[1] == true) {
		std::cout << "Sag\n";
	}

	this->mPlayerInputs[0] = clientPacket->movementButtons[0];
	this->mPlayerInputs[1] = clientPacket->movementButtons[1];
	this->mPlayerInputs[2] = clientPacket->movementButtons[2];
	this->mPlayerInputs[3] = clientPacket->movementButtons[3];
}

void TestObject::ClearInputs() {
	this->mPlayerInputs[0] = false;
	this->mPlayerInputs[1] = false;
	this->mPlayerInputs[2] = false;
	this->mPlayerInputs[3] = false;
}

int TestObject::GetPlayerID() {
	return mPlayerID;
}

void TestObject::MoveLeft(float dt) {
	GetPhysicsObject()->AddForce({ -50, 0,0 });
}

void TestObject::MoveRight(float dt) {
	GetPhysicsObject()->AddForce({ 50, 0,0 });
}