#include <iostream>
#include <sstream>

#include "DistributedGameServerManager.h"
#include "GameTimer.h"
#include "ServerWorldManager.h"

int ParsePortNumber(std::string& portStr) {
	int port;

	try {
		port = std::stoi(portStr);
		if (port < 0 || port > 65535) {
			throw std::out_of_range("Port number must be between 0 and 65535");
		}
	}
	catch (const std::invalid_argument& e) {
		std::cerr << "Invalid port number: " << portStr << std::endl;
		return 1;
	}
	catch (const std::out_of_range& e) {
		std::cerr << e.what() << std::endl;
		return 1;
	}

	return port;
}

std::vector<char> ipv4_to_char_array(const std::string& ip_str) {
	std::vector<std::string> ip_bytes;
	std::stringstream ss(ip_str);
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

int StartGameServer(int argc, char* argv[]) {

	std::cout << "\nCommand-line arguments:\n";
	for (int count = 0; count < argc; count++)
		std::cout << "  argv[" << count << "]   " << argv[count] << "\n";

	//FOR TEST
	bool isTesting = false;
	std::string testArgument = "127.0.0.1-2000-1";

	std::string argument = isTesting ? testArgument : argv[2];

	std::vector<size_t> separators;
	for (size_t pos = 0; (pos = argument.find('-', pos)) != std::string::npos; ++pos) {
		separators.push_back(pos);
	}

	std::string ipAddress = argument.substr(0, separators[0]);
	std::string portStr = argument.substr(separators[0] + 1, separators[1] - separators[0] - 1);
	std::string serverIdStr = argument.substr(separators[1] + 1);
	std::string serverBorders = argument.substr(separators[2] + 1);

	int serverId = std::stoi(serverIdStr);

	int port = ParsePortNumber(portStr);

	std::vector<char> ipOctets = ipv4_to_char_array(ipAddress);

	// Now you have parsed IP address (e.g., store in a struct) and port number
	std::cout << "Parsed IP address: " << ipAddress << '\n';
	std::cout << "Parsed port number: " << port << '\n';
	std::cout << "Parsed server ID: " << serverId << '\n';
	std::cout << "Parsed server borders string: " << serverBorders << '\n';


	NCL::DistributedGameServer::DistributedGameServerManager* serverManager = new NCL::DistributedGameServer::DistributedGameServerManager(serverId, serverBorders);
	serverManager->StartDistributedGameServer(ipOctets[0], ipOctets[1], ipOctets[2], ipOctets[3], port);
	NCL::GameTimer timer;

	bool isServerRunning = true;

	timer.GetTimeDeltaSeconds(); //Clear the timer so we don't get a larget first dt!
	while (isServerRunning) {
		timer.Tick();
		float dt = timer.GetTimeDeltaSeconds();
		if (dt > 0.1f) {
			std::cout << "Skipping large time delta" << '\n';
			continue; //must have hit a breakpoint or something to have a 1 second frame time!
		}

		if (serverManager->GetGameStarted()) {
			serverManager->GetServerWorldManager()->Update(dt);
		}

		std::chrono::steady_clock::time_point start;
		std::chrono::steady_clock::time_point end;
		std::chrono::duration<double, std::milli> timeTaken;

		start = std::chrono::high_resolution_clock::now();
		serverManager->UpdateGameServerManager(dt);
		end = std::chrono::high_resolution_clock::now();
		timeTaken = end - start;
		
		//std::cout << "Network Update Time: " << timeTaken << "\n";
	}

	return 0;
}
