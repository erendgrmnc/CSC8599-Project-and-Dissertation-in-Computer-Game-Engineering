#pragma once
#include "NetworkBase.h"

namespace NCL::CSC8503 {
	struct PhysicsServerMiddlewareDataPacket;
}

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

		// Override the path of the physics-server executable this midware spawns.
		// Defaults to the legacy "./DistributedPhysicsServer/EntryPoint.exe"; the
		// GUI launcher points it at the deployed game-server exe.
		void SetServerExePath(const std::string& path);

		// When headless, spawned game servers are launched windowless with --headless
		// and their stdout is forwarded to this midware's stdout (so the launcher sees
		// it). Mirrors the midware's own run mode.
		void SetHeadless(bool headless);
	protected:
		std::string serverManagerIpAddress;
		int distributedManagerPort;
		int mMidwareID;
		std::string mServerExePath;
		bool mHeadless = false;
		CSC8503::GameClient* mDistributedManagerClient;

		void RegisterDistributedManagerClientPackets();
		void HandleRunInstancePacket(CSC8503::RunDistributedPhysicsServerInstancePacket* packet);
		void StartPhysicsServerInstance(int distributedManagerPort, int physicsServerID, int gameInstanceID, std::string& borderStr);
		void ExecutePhysicsServerProgram(const std::string& programPath, const std::string& arguments, int serverId);
		void HandleMidwareDataPacket(CSC8503::PhysicsServerMiddlewareDataPacket* packet);

		void SendMidwareConnectedPacket(const std::string& ipAddress);
	};
}
