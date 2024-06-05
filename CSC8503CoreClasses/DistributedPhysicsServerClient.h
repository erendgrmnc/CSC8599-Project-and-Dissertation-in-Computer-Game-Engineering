#pragma once
#include "GameClient.h"
#ifdef USEGL
#include <cstdint>

namespace NCL {
	namespace Networking {
		class DistributedPhysicsServerClient : public CSC8503::GameClient {
		public:
			DistributedPhysicsServerClient();
			~DistributedPhysicsServerClient();

			bool UpdatePhysicsServer();
			bool UpdateClient() override;
			void DisconnectFromDistributedServerManager();
			void RegisterOnConnectedToDistributedManagerEvent(const std::function<void()>& callback);
		protected:
			std::vector<std::function<void()>> mOnAllClientsAreConnected;
			void TriggerOnAllClientsAreConnectedEvents() const;
		};
	}
}

#endif