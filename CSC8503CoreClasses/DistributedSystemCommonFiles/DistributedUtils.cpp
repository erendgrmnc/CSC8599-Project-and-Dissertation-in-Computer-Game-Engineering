#include <string>
#include "DistributedUtils.h"
#include "DistributedSystemCommonFiles/DistributedPhysicsServerDto.h"

NCL::DistributedPhysicsServerData* NCL::DistributedUtils::CreatePhysicsServerData(const std::string& ipAddress, int serverId) {
	NCL::DistributedPhysicsServerData* data = new NCL::DistributedPhysicsServerData();
	data->ipAddress = ipAddress;
	data->serverId = serverId;

	return data;
}
