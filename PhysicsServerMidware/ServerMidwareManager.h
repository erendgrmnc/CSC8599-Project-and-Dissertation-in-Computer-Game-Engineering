#pragma once
#include "NetworkBase.h"

namespace NCL {
	namespace CSC8503 {
		struct RunDistributedPhysicsServerInstancePacket;
		class GameClient;
	}

	class ServerMidwareManager : public PacketReceiver {
	public:
		ServerMidwareManager();
		~ServerMidwareManager();

		void ConnectToDistributedManager(std::string& ipAddress, int port);
		void ReceivePacket(int type, GamePacket* payload, int source) override;
		void Update(float dt);
	protected:
		std::string serverManagerIpAddress;
		int distributedManagerPort;
		CSC8503::GameClient* mDistributedManagerClient;

		void RegisterDistributedManagerClientPackets();
		void HandleRunInstancePacket(CSC8503::RunDistributedPhysicsServerInstancePacket* packet);
		void StartPhysicsServerInstance(int distributedManagerPort, int physicsServerID, int gameInstanceID, std::string& borderStr);
		void ExecutePhysicsServerProgram(const std::string& programPath, const std::string& arguments, int serverId);
	};
}
