#include "command.hpp"
#include "rdb.hpp"
#include <algorithm>
#include <chrono>
#include <sys/socket.h>

// Parse one complete RESP command from buf starting at 'start'.
// Returns bytes consumed, or 0 if the command is incomplete.
size_t try_parse_command(const std::string& buf, size_t start, std::vector<std::string>& args){
	args.clear();
	if (start >= buf.size()) return 0;

	if (buf[start] == '*'){
		// RESP array: *N\r\n $len\r\n data\r\n ...
		size_t pos = start;
		size_t nl = buf.find("\r\n", pos);
		if (nl == std::string::npos) return 0;
		int num_args = std::stoi(buf.substr(pos + 1, nl - pos - 1));
		pos = nl + 2;

		for (int i = 0; i < num_args; i++){
			nl = buf.find("\r\n", pos);
			if (nl == std::string::npos) return 0;
			if (pos >= buf.size() || buf[pos] != '$') return 0;
			int len = std::stoi(buf.substr(pos + 1, nl - pos - 1));
			pos = nl + 2;
			if (pos + len + 2 > buf.size()) return 0; // need len bytes + \r\n
			args.push_back(buf.substr(pos, len));
			pos += len + 2;
		}
		return pos - start;
	}
	else{
		// Inline command: COMMAND arg1 arg2\r\n (used by redis-benchmark for PING)
		size_t nl = buf.find("\r\n", start);
		if (nl == std::string::npos) return 0;
		std::string line = buf.substr(start, nl - start);
		if (line.empty()) return nl + 2 - start; // skip blank lines

		size_t pos = 0;
		while (pos < line.size()){
			while (pos < line.size() && line[pos] == ' ') pos++; // skip spaces
			if (pos >= line.size()) break;
			size_t end = line.find(' ', pos);
			if (end == std::string::npos) end = line.size();
			args.push_back(line.substr(pos, end - pos));
			pos = end;
		}
		return nl + 2 - start;
	}
}

// Encode args back into a RESP array: *N\r\n $len\r\n arg\r\n ...
std::string encode_resp(const std::vector<std::string>& args){
	std::string out = "*" + std::to_string(args.size()) + "\r\n";
	for (const auto& arg : args){
		out += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
	}
	return out;
}

// Forward a write command to all connected replicas
void propagate_to_replicas(const std::vector<std::string>& args){
	std::string resp = encode_resp(args);
	for (auto& [rfd, rclient] : clients){
		if (rclient.is_replica){
			send(rfd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
		}
	}
	// Track how many bytes of replication stream we've produced
	master_repl_offset += resp.size();
}

// Execute a parsed command and return the RESP response
std::string process_command(int fd, Client& client, std::vector<std::string>& args){
	std::string& cmd = args[0];
	std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper); // case-insensitive commands

	// Replicas reject write commands
	if (!replicaof_host.empty() && fd != master_fd && (cmd == "SET" || cmd == "DEL")){
		return "-READONLY You can't write against a read only replica\r\n";
	}

	if (cmd == "PING"){
		return "+PONG\r\n";
	}
	else if (cmd == "ECHO" && args.size() >= 2){
		return "$" + std::to_string(args[1].size()) + "\r\n" + args[1] + "\r\n";
	}
	else if (cmd == "SET" && args.size() >= 3){
		auto expiry_time = std::chrono::system_clock::time_point::max(); // no expiry by default
		if (args.size() >= 5){
			std::string flag = args[3];
			std::transform(flag.begin(), flag.end(), flag.begin(), ::toupper);
			if (flag == "PX"){
				int px = std::stoi(args[4]);
				expiry_time = std::chrono::system_clock::now() + std::chrono::milliseconds(px);
			}
		}
		db[args[1]] = Entry{args[2], expiry_time};
		propagate_to_replicas(args); // forward SET to all replicas
		return "+OK\r\n";
	}
	else if (cmd == "GET" && args.size() >= 2){
		auto it = db.find(args[1]);
		if (it == db.end()){
			return "$-1\r\n";
		}
		else if (std::chrono::system_clock::now() > it->second.expires_at){
			db.erase(it); // lazy expiration
			return "$-1\r\n";
		}
		else{
			return "$" + std::to_string(it->second.value.size()) + "\r\n" + it->second.value + "\r\n";
		}
	}
	else if (cmd == "CONFIG" && args.size() >= 3){
		std::string subcmd = args[1];
		std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::toupper);
		if (subcmd == "GET"){ // dir/dbfilename are set once at startup, no locking needed
			std::string& param = args[2];
			std::string value;
			if (param == "dir") value = dir;
			else if (param == "dbfilename") value = dbfilename;
			std::string response = "*2\r\n";
			response += "$" + std::to_string(param.size()) + "\r\n" + param + "\r\n";
			response += "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
			return response;
		}
	}
	else if (cmd == "KEYS" && args.size() >= 2 && args[1] == "*"){
		std::vector<std::string> valid_keys;
		auto now = std::chrono::system_clock::now();
		for (auto it = db.begin(); it != db.end();){
			if (now > it->second.expires_at){
				it = db.erase(it); // remove expired keys
			}
			else{
				valid_keys.push_back(it->first);
				it++;
			}
		}
		std::string response = "*" + std::to_string(valid_keys.size()) + "\r\n";
		for (const auto& key : valid_keys){
			response += "$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
		}
		return response;
	}

	else if (cmd == "SAVE"){
		const std::string rdb_path = dir.empty() ? "dump.rdb" : dir + "/" + dbfilename;
		save_rdb_file(rdb_path, db);
		return "+OK\r\n";
	}

	else if (cmd == "REPLCONF"){
		// Handshake from a connecting replica — just acknowledge
		return "+OK\r\n";
	}

	else if (cmd == "PSYNC" && args.size() >= 3){
		// Full resync: send FULLRESYNC header + RDB snapshot directly on the fd
		std::string header = "+FULLRESYNC " + master_replid + " "
			+ std::to_string(master_repl_offset) + "\r\n";
		send(fd, header.c_str(), header.size(), MSG_NOSIGNAL);

		// Build the RDB and send as $<len>\r\n<bytes> (no trailing \r\n)
		std::vector<uint8_t> rdb = build_rdb(db);
		std::string prefix = "$" + std::to_string(rdb.size()) + "\r\n";
		send(fd, prefix.c_str(), prefix.size(), MSG_NOSIGNAL);
		send(fd, rdb.data(), rdb.size(), MSG_NOSIGNAL);

		// Mark this client as a replica for future write propagation
		client.is_replica = true;
		return ""; // already sent everything directly
	}

	else if (cmd == "INFO" && args.size() >= 2){
		std::string section = args[1];
		std::transform(section.begin(), section.end(), section.begin(), ::toupper);
		if (section == "REPLICATION"){
			// Return role, replid, and offset as a bulk string
			std::string role = replicaof_host.empty() ? "master" : "slave";
			std::string info = "role:" + role + "\r\n";
			info += "master_replid:" + master_replid + "\r\n";
			info += "master_repl_offset:" + std::to_string(master_repl_offset) + "\r\n";
			return "$" + std::to_string(info.size()) + "\r\n" + info + "\r\n";
		}
	}

	return "-ERR unknown command '" + args[0] + "'\r\n";
}
