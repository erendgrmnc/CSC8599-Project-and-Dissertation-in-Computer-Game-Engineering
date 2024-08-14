#pragma once
namespace NCL{
	struct GameBorder {
		double maxX = 0.f;
		double minX = 0.f;
		double	 minZ = 0.f;
		double maxZ = 0.f;

		GameBorder(double minX, double maxX, double minZ, double maxZ) {
			this->maxX = maxX;
			this->minX = minX;
			this->minZ = minZ;
			this->maxZ = maxZ;
		}
	};
	struct DistributedPhysicsServerData {
		bool isServerStarted = false;
		bool isAllClientsConnectedToServer = false;

		std::string ipAddress;
		int dataSenderPort;
		int serverId;
	};
}