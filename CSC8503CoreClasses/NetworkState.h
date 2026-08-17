#ifdef USEGL
#pragma once

namespace NCL {
	using namespace Maths;
	namespace CSC8503 {
		class GameObject;
		// Deliberately trivially copyable, and it must stay that way.
		//
		// This type is embedded BY VALUE in FullPacket and StartSimulatingObjectPacket,
		// both of which the ENet path memcpy's onto the wire. It previously declared a
		// `virtual ~NetworkState()` with an empty body while nothing derived from it,
		// which gave every instance a vtable pointer - so every full snapshot (10 Hz x
		// every object) and every handoff carried 8 bytes of process-local pointer
		// across the network. It happened to work because both ends are the same binary
		// and a field-wise copy never touches the vptr, but it is undefined behaviour
		// by the letter and it violates the wire-protocol rule the rest of this layer
		// follows.
		//
		// Do not add virtual functions, a user-provided destructor, or a non-trivially-
		// copyable member. tools/InteractionTests pins this.
		class NetworkState	{
		public:
			NetworkState() = default;
			~NetworkState() = default;

			int GetNetworkState() { return stateID; }
			void SetNetworkState(int newID) { stateID = newID; }

			Quaternion GetObjectOrientation() { return orientation; }
			void SetOrientation(Quaternion newOri) { orientation = newOri; }

			Vector3 GetObjectPos() { return position; }
			void SetPosition(Vector3 newPos) { position = newPos; }

			Vector3 GetPredictedPos() { return predictedPosition; }
			void SetPredictedPos(Vector3 newPredictedPos) { predictedPosition = newPredictedPos; }

			Quaternion GetPredictedOrientation() { return predictedOrientation; }
			void SetPredictedOrientation(Quaternion newPredictedOrientation) { predictedOrientation = newPredictedOrientation; }

			Vector3		position;
			Vector3		predictedPosition;
			Quaternion	orientation;
			Quaternion  predictedOrientation;
			// In-class initialiser rather than a constructor body: a default member
			// initialiser makes the DEFAULT constructor non-trivial, which is fine -
			// trivial copyability only constrains copy/move/destroy.
			int			stateID = 0;
		};
	}
}

#endif