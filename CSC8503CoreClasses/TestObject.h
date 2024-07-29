#pragma once
#include <vector>

#include "GameObject.h"

namespace NCL {
    namespace CSC8503 {
        class StateMachine;
        class State;
        class StateTransition;
        class TestObject : public GameObject {
        public:
            TestObject();
            TestObject(std::vector<State*>& states, std::vector<StateTransition*>& stateTransitions);
            ~TestObject();

            virtual void Update(float dt);
            void AddStates(vector<State*>& states);
            void AddStateTransitions(std::vector<StateTransition*>& stateTransitions);

        protected:
            bool mIsForceAdded = false;
            void MoveLeft(float dt);
            void MoveRight(float dt);

            StateMachine* stateMachine;
        };
    }
}