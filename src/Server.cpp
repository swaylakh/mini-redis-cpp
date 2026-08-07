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
#include "rdb.hpp"

std::string dir, dbfilename; // Directory and database filename
std::unordered_map<std::string, Entry> db; // database to store key-value pairs

// Per-client read buffer for partial RESP parsing
struct Client {
	std::string buf;
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

// Process a parsed command and return the RESP response
std::string process_command(std::vector<std::string>& args){
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

	return "-ERR unknown command '" + args[0] + "'\r\n";
}

int main(int argc, char **argv){
	// Flush after every std::cout / std::cerr
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;
	for (int i = 1; i < argc; i++){ // Persistence options
		if (std::string(argv[i]) == "--dir" && i + 1 < argc)
			dir = argv[i + 1];
		if (std::string(argv[i]) == "--dbfilename" && i + 1 < argc)
			dbfilename = argv[i + 1];
	}
	const std::string rdb_path = dir + "/" + dbfilename;
	load_rdb_file(rdb_path, db);

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
	server_addr.sin_port = htons(6379);

	if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0){
		std::cerr << "Failed to bind to port 6379\n";
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

	std::cout << "Waiting for a client to connect...\n";

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
						outbuf += process_command(args);
					}
					pos += consumed;
				}

				// Remove consumed bytes from the buffer
				if (pos > 0) client.buf.erase(0, pos);

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
