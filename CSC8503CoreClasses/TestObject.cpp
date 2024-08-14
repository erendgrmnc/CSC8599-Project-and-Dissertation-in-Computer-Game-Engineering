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
		//std::cout << "Moving Left.\n";
		MoveLeft(dt);
	}
	if (mPlayerInputs[1]) {

		//std::cout << "Moving Right.\n";
		MoveRight(dt);
	}

	if (mPlayerInputs[2]) {

		//std::cout << "Moving Down.\n";
		MoveDown(dt);
	}

	if (mPlayerInputs[3]) {
		//std::cout << "Moving Up.\n";
		MoveUp(dt);
	}
}

void TestObject::ReceiveClientInputs(ClientPlayerInputPacket* clientPacket) {
	this->mPlayerInputs[0] = clientPacket->playerInputs.movementButtons[0];
	this->mPlayerInputs[1] = clientPacket->playerInputs.movementButtons[1];
	this->mPlayerInputs[2] = clientPacket->playerInputs.movementButtons[2];
	this->mPlayerInputs[3] = clientPacket->playerInputs.movementButtons[3];
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

void TestObject::MoveUp(float dt) {
	GetPhysicsObject()->AddForce({ 0,0,50 });
}

void TestObject::MoveDown(float dt) {
	GetPhysicsObject()->AddForce({ 0,0,-50 });
}