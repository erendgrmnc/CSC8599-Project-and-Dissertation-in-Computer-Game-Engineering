#pragma once
#include <string>

namespace NCL {
	struct DistributedPhysicsServerData;

	class DistributedUtils {
	public:
		 static NCL::DistributedPhysicsServerData* CreatePhysicsServerData(const std::string& ipAddress, int serverId, int gameInstanceID);
		 static std::vector<char> ConvertIpStrToCharArr(std::string ipAddress);
	protected:
	};
}
