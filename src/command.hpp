#ifndef COMMAND_HPP
#define COMMAND_HPP

#include <string>
#include <vector>
#include "server.hpp"

// Parse one RESP command from buf at offset 'start', returns bytes consumed (0 if incomplete)
size_t try_parse_command(const std::string& buf, size_t start, std::vector<std::string>& args);

// Encode args back into RESP wire format: *N\r\n $len\r\n arg\r\n ...
std::string encode_resp(const std::vector<std::string>& args);

// Forward a write command to all connected replicas
void propagate_to_replicas(const std::vector<std::string>& args);

// Execute a parsed command and return the RESP response
std::string process_command(int fd, Client& client, std::vector<std::string>& args);

#endif
