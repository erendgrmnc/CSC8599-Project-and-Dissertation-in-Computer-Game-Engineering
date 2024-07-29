#include "TestObject.h"

#include <vector>

#include "StateTransition.h"
#include "StateMachine.h"
#include "State.h"
#include "PhysicsObject.h"

using namespace NCL;
using namespace CSC8503;

NCL::CSC8503::TestObject::TestObject() {
	stateMachine = new StateMachine();
	State* stateA = new State([&](float dt, GameObject* obj)-> void {
		this->MoveLeft(dt);
		});

	State* stateB = new State([&](float dt, GameObject* obj)->void {
		this->MoveRight(dt);
		});

	stateMachine->AddState(stateA);
	stateMachine->AddState(stateB);

	StateTransition* stateTransitionAB = new StateTransition(stateA, stateB, [&]()-> bool {
		if (this->GetTransform().GetPosition().x > 100) {
			return true;
		}
		return false;
		});

	StateTransition* stateTransitionBA = new StateTransition(stateB, stateA, [&]()->bool {
		if (this->GetTransform().GetPosition().x < -100.f) {
			return true;
		}
		return false;
		});

	stateMachine->AddTransition(stateTransitionAB);
	stateMachine->AddTransition(stateTransitionBA);

}

NCL::CSC8503::TestObject::TestObject(vector<State*>& states, std::vector<StateTransition*>& stateTransitions) {
	stateMachine = new StateMachine();

	for (const auto& state : states) {
		stateMachine->AddState(state);
	}
	for (const auto& stateTransition : stateTransitions) {
		stateMachine->AddTransition(stateTransition);
	}
}

TestObject::~TestObject() {
	delete stateMachine;
}

void TestObject::Update(float dt) {
	stateMachine->Update(dt);
}

void NCL::CSC8503::TestObject::AddStates(std::vector<State*>& states) {
	for (const auto& state : states) {
		stateMachine->AddState(state);
	}
}

void NCL::CSC8503::TestObject::AddStateTransitions(std::vector<StateTransition*>& stateTransitions) {
	for (const auto& stateTransition : stateTransitions) {
		stateMachine->AddTransition(stateTransition);
	}
}

void TestObject::MoveLeft(float dt) {
	GetPhysicsObject()->AddForce({ -40, 0,0 });
}

void TestObject::MoveRight(float dt) {
	GetPhysicsObject()->AddForce({ 40, 0,0 });
}