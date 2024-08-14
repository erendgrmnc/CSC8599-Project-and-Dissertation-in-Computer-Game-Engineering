#pragma once
#include <string>

namespace NCL {
	struct DistributedPhysicsServerData;

	class DistributedUtils {
	public:
		 static NCL::DistributedPhysicsServerData* CreatePhysicsServerData(const std::string& ipAddress, int serverId);
	protected:
	};
}
