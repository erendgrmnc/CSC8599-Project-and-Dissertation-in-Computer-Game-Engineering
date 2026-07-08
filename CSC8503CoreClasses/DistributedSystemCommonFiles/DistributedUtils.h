#pragma once
#include <string>

namespace NCL {
	struct DistributedPhysicsServerData;

	class DistributedUtils {
	public:
		 static NCL::DistributedPhysicsServerData* CreatePhysicsServerData(const std::string& ipAddress, int serverId, int gameInstanceID);
		 static std::vector<char> ConvertIpStrToCharArr(std::string ipAddress);
		 static std::string GetMachineIPV4Address();

		 // Parses "minX/maxX|minZ/maxZ" into world bounds. Returns false (leaving the
		 // outputs untouched) if the string is malformed.
		 static bool ParseBorderString(const std::string& borderStr, float& minX, float& maxX, float& minZ, float& maxZ);
	protected:
	};
}
