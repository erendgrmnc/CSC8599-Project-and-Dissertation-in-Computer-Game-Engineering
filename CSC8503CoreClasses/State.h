#pragma once
#include "GameObject.h"

namespace NCL {
	namespace CSC8503 {
		typedef std::function<void(float, GameObject*)> StateUpdateFunction;

		class  State		{
		public:
			State() {}
			State(StateUpdateFunction someFunc) {
				func		= someFunc;
			}
			void Update(float dt, GameObject* object = nullptr)  {
				if (func != nullptr) {
					func(dt, object);
				}
			}
		protected:
			StateUpdateFunction func;
		};
	}
}
