#include <string>
#include "DistributedUtils.h"

#include <sstream>

#include "DistributedSystemCommonFiles/DistributedPhysicsServerDto.h"

NCL::DistributedPhysicsServerData* NCL::DistributedUtils::CreatePhysicsServerData(const std::string& ipAddress, int serverId, int gameInstanceID) {
	NCL::DistributedPhysicsServerData* data = new NCL::DistributedPhysicsServerData(serverId, gameInstanceID, ipAddress);
	return data;
}

std::vector<char> NCL::DistributedUtils::ConvertIpStrToCharArr(std::string ipAddress) {
	std::vector<std::string> ip_bytes;
	std::stringstream ss(ipAddress);
	std::string segment;

	while (std::getline(ss, segment, '.')) {
		ip_bytes.push_back(segment);
	}

	if (ip_bytes.size() != 4) {
		throw std::invalid_argument("Invalid IPv4 address format");
	}

	std::vector<char> ip_packed;
	for (const std::string& byte_str : ip_bytes) {
		int byte_value = std::stoi(byte_str);
		if (byte_value < 0 || byte_value > 255) {
			throw std::invalid_argument("Invalid IPv4 address format");
		}
		ip_packed.push_back(static_cast<char>(byte_value));
	}

	return ip_packed;
}
