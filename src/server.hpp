#ifndef SERVER_HPP
#define SERVER_HPP

#include <string>
#include <cstdint>
#include <unordered_map>
#include "rdb.hpp"

// Per-client state
struct Client {
	std::string buf;         // read buffer for partial RESP parsing
	bool is_replica = false; // true after PSYNC — receives propagated writes
};

// Shared globals — defined in Server.cpp, accessible from other translation units
extern std::string dir, dbfilename;
extern int server_port;
extern std::unordered_map<std::string, Entry> db;
extern std::unordered_map<int, Client> clients;

// Replication state
extern std::string master_replid;
extern int64_t master_repl_offset;
extern std::string replicaof_host;
extern int replicaof_port;
extern int master_fd;

#endif
