#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <iostream>
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

std::string NCL::DistributedUtils::GetMachineIPV4Address() {
    std::string ipAddress = "Unable to get IP Address";
    WSADATA wsaData;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return ipAddress;
    }

    char ac[80];
    if (gethostname(ac, sizeof(ac)) == SOCKET_ERROR) {
        WSACleanup();
        return ipAddress;
    }

    struct addrinfo hints = { 0 };
    hints.ai_family = AF_INET; // IPv4
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    if (getaddrinfo(ac, nullptr, &hints, &result) != 0) {
        WSACleanup();
        return ipAddress;
    }

    for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        struct sockaddr_in* sockaddr_ipv4 = reinterpret_cast<struct sockaddr_in*>(ptr->ai_addr);
        wchar_t ipStr[INET_ADDRSTRLEN];
        if (InetNtop(AF_INET, &(sockaddr_ipv4->sin_addr), ipStr, INET_ADDRSTRLEN) != nullptr) {
            // Convert the wide string to a standard string
            std::wstring ws(ipStr);
            ipAddress = std::string(ws.begin(), ws.end());
            break;  // Get the first IP address and break
        }
    }

    freeaddrinfo(result);
    WSACleanup();
    return ipAddress;
}
