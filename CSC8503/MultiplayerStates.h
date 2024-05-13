#pragma once


#include "PushdownState.h"

namespace NCL {
	namespace CSC8503 {
		class MultiplayerGameScene;


		class MultiplayerLobby : public PushdownState {
		public:
			MultiplayerLobby(MultiplayerGameScene* currentGameState);

			PushdownResult OnUpdate(float dt, PushdownState** newState)  override;

			void OnAwake() override;

			MultiplayerGameScene* mGameSceneManager;
		};

		class InitialisingMultiplayerLevel : public PushdownState {
		public:
			InitialisingMultiplayerLevel(MultiplayerGameScene* currentGameState);

			PushdownResult OnUpdate(float dt, PushdownState** newState)  override;

			void OnAwake() override;

			MultiplayerGameScene* mGameSceneManager;
		};

		class PlayingMultiplayerLevel : public PushdownState {
		public:
			PlayingMultiplayerLevel(MultiplayerGameScene* currentGameState) {
				mGameSceneManager = currentGameState;
			}

			PushdownResult OnUpdate(float dt, PushdownState** newState)  override;

			void OnAwake() override;

			MultiplayerGameScene* mGameSceneManager;
		};

		class MultiplayerVictory : public PushdownState {
		public:
			MultiplayerVictory(MultiplayerGameScene* currentGameState) {
				mGameSceneManager = currentGameState;
			}

			PushdownResult OnUpdate(float dt, PushdownState** newState)  override;

			void OnAwake() override;

			MultiplayerGameScene* mGameSceneManager;
		};

		class MultiplayerDefeat : public PushdownState {
		public:
			MultiplayerDefeat(MultiplayerGameScene* currentGameState);

			PushdownResult OnUpdate(float dt, PushdownState** newState)  override;

			void OnAwake() override;

			MultiplayerGameScene* mGameSceneManager;
		};
	}
}