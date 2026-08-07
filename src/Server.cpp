#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <fstream>
#include <random>
#include "rdb.hpp"

std::string dir, dbfilename; // Directory and database filename
int server_port = 6379; // port this server listens on
std::unordered_map<std::string, Entry> db; // database to store key-value pairs

// Replication state
std::string master_replid; // 40-char hex ID, unique per master instance
int64_t master_repl_offset = 0; // bytes of replication stream produced
std::string replicaof_host; // empty = master mode, set = replica mode
int replicaof_port = 0;
int master_fd = -1; // fd of connection to master (replica mode only)

// Per-client state
struct Client {
	std::string buf;       // read buffer for partial RESP parsing
	bool is_replica = false; // true after PSYNC — receives propagated writes
};
std::unordered_map<int, Client> clients; // fd -> client state

void set_nonblocking(int fd){
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Try to parse one complete RESP command from buf starting at 'start'.
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

// Process a parsed command and return the RESP response
std::string process_command(int fd, Client& client, std::vector<std::string>& args){
	std::string& cmd = args[0];
	std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper); // case-insensitive commands

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
		// Handshake from a connecting replica — just acknowledge for now
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

// Read one line from a blocking socket (up to \r\n)
std::string read_line(int fd){
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
std::string send_command(int fd, const std::vector<std::string>& args){
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

int main(int argc, char **argv){
	// Flush after every std::cout / std::cerr
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;
	for (int i = 1; i < argc; i++){
		if (std::string(argv[i]) == "--dir" && i + 1 < argc)
			dir = argv[i + 1];
		if (std::string(argv[i]) == "--dbfilename" && i + 1 < argc)
			dbfilename = argv[i + 1];
		if (std::string(argv[i]) == "--port" && i + 1 < argc)
			server_port = std::stoi(argv[i + 1]);
		if (std::string(argv[i]) == "--replicaof" && i + 2 < argc){
			replicaof_host = argv[i + 1];
			replicaof_port = std::stoi(argv[i + 2]);
		}
	}

	// Load RDB from disk only in master mode
	if (replicaof_host.empty() && !dir.empty()){
		const std::string rdb_path = dir + "/" + dbfilename;
		load_rdb_file(rdb_path, db);
	}

	// Generate a random 40-char hex replication ID
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<int> dist(0, 15);
	const char hex[] = "0123456789abcdef";
	master_replid.resize(40);
	for (int i = 0; i < 40; i++) master_replid[i] = hex[dist(gen)];

	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0){
		std::cerr << "Failed to create server socket\n";
		return 1;
	}

	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0){
		std::cerr << "setsockopt failed\n";
		return 1;
	}

	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(server_port);

	if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0){
		std::cerr << "Failed to bind to port " << server_port << "\n";
		return 1;
	}

	if (listen(server_fd, 128) != 0){ // higher backlog for epoll
		std::cerr << "listen failed\n";
		return 1;
	}

	set_nonblocking(server_fd); // accept() must not block the event loop

	int epoll_fd = epoll_create1(0); // create the epoll instance
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev); // watch for new connections

	// If we're a replica, connect to master and do the handshake
	if (!replicaof_host.empty()){
		master_fd = connect_to_master(replicaof_host, replicaof_port);
		if (master_fd < 0) return 1;
		// Add master fd to epoll — incoming data is propagated commands
		set_nonblocking(master_fd);
		ev.events = EPOLLIN;
		ev.data.fd = master_fd;
		epoll_ctl(epoll_fd, EPOLL_CTL_ADD, master_fd, &ev);
		clients[master_fd] = Client{};
	}

	std::cout << "Listening on port " << server_port << " (role: "
		<< (replicaof_host.empty() ? "master" : "replica") << ")\n";

	const int MAX_EVENTS = 64;
	struct epoll_event events[MAX_EVENTS];
	char read_buf[4096];

	while (true){ // single-threaded event loop
		int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1); // block until something happens
		for (int i = 0; i < nfds; i++){
			if (events[i].data.fd == server_fd){
				// New connection(s) — accept all pending
				while (true){
					struct sockaddr_in client_addr;
					socklen_t client_len = sizeof(client_addr);
					int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
					if (client_fd < 0) break; // EAGAIN: no more pending connections
					set_nonblocking(client_fd);
					ev.events = EPOLLIN;
					ev.data.fd = client_fd;
					epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
					clients[client_fd] = Client{};
				}
			}
			else{
				// Data from an existing client
				int fd = events[i].data.fd;
				ssize_t n = read(fd, read_buf, sizeof(read_buf));
				if (n <= 0){
					// Client disconnected or error
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
					close(fd);
					clients.erase(fd);
					continue;
				}

				Client& client = clients[fd];
				client.buf.append(read_buf, n);

				// Parse and process as many complete commands as possible
				std::string outbuf;
				size_t pos = 0;
				while (pos < client.buf.size()){
					std::vector<std::string> args;
					size_t consumed = try_parse_command(client.buf, pos, args);
					if (consumed == 0) break; // incomplete command, wait for more data
					if (!args.empty()){
						outbuf += process_command(fd, client, args);
					}
					pos += consumed;
				}

				// Remove consumed bytes from the buffer
				if (pos > 0) client.buf.erase(0, pos);

				// Don't reply to the master — propagated commands are fire-and-forget
				if (fd == master_fd) continue;

				// Send all responses in one syscall (batched pipelining)
				if (!outbuf.empty()){
					send(fd, outbuf.c_str(), outbuf.size(), MSG_NOSIGNAL);
				}
			}
		}
	}

	close(server_fd);
	close(epoll_fd);
	return 0;
}
