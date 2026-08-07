#ifndef REPLICATION_HPP
#define REPLICATION_HPP

#include <string>

// Connect to a master server, run the PSYNC handshake, load the RDB snapshot.
// Returns the master's fd (caller adds it to epoll), or -1 on failure.
int connect_to_master(const std::string& host, int port);

#endif
