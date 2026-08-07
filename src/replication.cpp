#include "replication.hpp"
#include "server.hpp"
#include "command.hpp"
#include "rdb.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// Read one line from a blocking socket (up to \r\n)
static std::string read_line(int fd){
	std::string line;
	char c;
	while (::read(fd, &c, 1) == 1){
		line += c;
		if (line.size() >= 2 && line.substr(line.size()-2) == "\r\n"){
			line.resize(line.size()-2); // strip \r\n
			return line;
		}
	}
	return line;
}

// Send a RESP command and read one line back (blocking, used during handshake)
static std::string send_command(int fd, const std::vector<std::string>& args){
	std::string resp = encode_resp(args);
	::send(fd, resp.c_str(), resp.size(), 0);
	return read_line(fd);
}

// Connect to master, run the handshake, receive the RDB snapshot
int connect_to_master(const std::string& host, int port){
	// Create a TCP connection to the master
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
		std::cerr << "Failed to connect to master at " << host << ":" << port << "\n";
		return -1;
	}
	std::cout << "Connected to master at " << host << ":" << port << "\n";

	// Step 1: PING
	std::string reply = send_command(fd, {"PING"});
	std::cout << "  PING → " << reply << "\n";

	// Step 2: REPLCONF listening-port
	reply = send_command(fd, {"REPLCONF", "listening-port", std::to_string(server_port)});
	std::cout << "  REPLCONF port → " << reply << "\n";

	// Step 3: REPLCONF capa psync2
	reply = send_command(fd, {"REPLCONF", "capa", "psync2"});
	std::cout << "  REPLCONF capa → " << reply << "\n";

	// Step 4: PSYNC ? -1 (request full resync)
	std::string psync = encode_resp({"PSYNC", "?", "-1"});
	::send(fd, psync.c_str(), psync.size(), 0);

	// Read the +FULLRESYNC line
	reply = read_line(fd);
	std::cout << "  PSYNC → " << reply << "\n";

	// Read the $<len> prefix
	std::string rdb_prefix = read_line(fd);
	int rdb_len = std::stoi(rdb_prefix.substr(1)); // skip the '$'
	std::cout << "  RDB size: " << rdb_len << " bytes\n";

	// Read exactly rdb_len bytes of RDB data
	std::vector<uint8_t> rdb_data(rdb_len);
	int total = 0;
	while (total < rdb_len){
		int n = ::read(fd, rdb_data.data() + total, rdb_len - total);
		if (n <= 0) break;
		total += n;
	}

	// Write to a temp file and load it (reuses existing RDB parser)
	{
		std::ofstream tmp("/tmp/replica_rdb.tmp", std::ios::binary);
		tmp.write(reinterpret_cast<const char*>(rdb_data.data()), rdb_data.size());
		tmp.close();
	}
	load_rdb_file("/tmp/replica_rdb.tmp", db);
	std::cout << "  Loaded " << db.size() << " keys from master\n";

	return fd;
}
