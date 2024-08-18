#pragma once
#include <vector>
#ifndef DISTRIBUTEDSYSTEMACTIVE
#include "../FMODCoreAPI/includes/fmod.hpp"



using namespace FMOD;
namespace NCL {
	namespace CSC8503 {
		class SoundObject {
		public:
			SoundObject(Channel* channel);
			SoundObject();
			~SoundObject();

			Channel* GetChannel();

			void TriggerSoundEvent();

			void CloseDoorTriggered();

			void LockDoorTriggered();

			bool GetisTiggered();

			bool GetIsClosed();

			bool GetIsLocked();

			void SetNotTriggered();

			void CloseDoorFinished();

			void LockDoorFinished();

			void Clear();

		protected:
			Channel* mChannel = nullptr;
			bool mIsTriggered;
			bool mIsClosed;
			bool mIsLocked;
		};
    }
}
#endif
