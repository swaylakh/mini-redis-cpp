#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <thread>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include "rdb.hpp"

std::string dir, dbfilename; // Directory and database filename
std::unordered_map<std::string, Entry> db; // database to store key-value pairs
std::shared_mutex db_mutex; // readers can share, writers get exclusive access

void handle_client(int client_fd){ // Function to handle client requests
	FILE *client_file = fdopen(client_fd, "r");
	char line[1024];
	while(fgets(line, sizeof(line), client_file)) {
			if (line[0] != '*') continue;
			int num_args = atoi(line + 1);
			std::vector<std::string> args;
			for(int i = 0; i < num_args; i++){
					if (!fgets(line, sizeof(line), client_file)) break; // read $len line
					if (line[0] != '$') break;
					int len = atoi(line + 1);
					std::string arg(len, '\0');
					if ((int)fread(&arg[0], 1, len, client_file) != len) break; // read exactly len bytes (binary-safe)
					fgetc(client_file); // consume \r
					fgetc(client_file); // consume \n
					args.push_back(std::move(arg));
			}
			if (args.empty()) continue; // skip malformed commands with no args
			std::string& cmd = args[0];
			std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper); // case-insensitive commands

			if (cmd == "PING"){
					send(client_fd, "+PONG\r\n", 7, 0);
			}
			else if (cmd == "ECHO" && args.size() >= 2){
					std::string& message = args[1];
					std::string response = "$" + std::to_string(message.size()) + "\r\n" + message + "\r\n";
					send(client_fd, response.c_str(), response.size(), 0);
			}
			else if (cmd == "SET" && args.size() >= 3){
					std::string& key = args[1];
					std::string& val = args[2];
					auto expiry_time = std::chrono::system_clock::time_point::max(); // no expiry by default
					if (args.size() >= 5){
							std::string& flag = args[3];
							std::transform(flag.begin(), flag.end(), flag.begin(), ::toupper);
							if (flag == "PX"){
									int px = std::stoi(args[4]);
									expiry_time = std::chrono::system_clock::now() + std::chrono::milliseconds(px);
							}
					}
					std::unique_lock lock(db_mutex); // exclusive lock for writing
					db[key] = Entry{val, expiry_time};
					lock.unlock();
					send(client_fd, "+OK\r\n", 5, 0);
			}
			else if (cmd == "GET" && args.size() >= 2){
					// unique_lock because GET may erase expired keys (lazy expiration)
					std::unique_lock lock(db_mutex);
					auto it = db.find(args[1]);
					if (it == db.end()){
							lock.unlock();
							send(client_fd, "$-1\r\n", 5, 0);
					}
					else if (std::chrono::system_clock::now() > it->second.expires_at){
							db.erase(it); // lazy expiration
							lock.unlock();
							send(client_fd, "$-1\r\n", 5, 0);
					}
					else{
							std::string response = "$" + std::to_string(it->second.value.size()) + "\r\n" + it->second.value + "\r\n";
							lock.unlock();
							send(client_fd, response.c_str(), response.size(), 0);
					}
			}
			else if (cmd == "CONFIG" && args.size() >= 3){
					std::string& subcmd = args[1];
					std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::toupper);
					if (subcmd == "GET"){ // no db lock needed, dir/dbfilename are set once at startup
							std::string& param = args[2];
							std::string value;
							if (param == "dir") value = dir;
							else if (param == "dbfilename") value = dbfilename;
							std::string response = "*2\r\n";
							response += "$" + std::to_string(param.size()) + "\r\n" + param + "\r\n";
							response += "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
							send(client_fd, response.c_str(), response.size(), 0);
					}
			}
			else if (cmd == "KEYS" && args.size() >= 2 && args[1] == "*"){
					// unique_lock because KEYS erases expired entries
					std::unique_lock lock(db_mutex);
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
					lock.unlock();
					std::string response = "*" + std::to_string(valid_keys.size()) + "\r\n";
					for (const auto& key : valid_keys){
							response += "$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
					}
					send(client_fd, response.c_str(), response.size(), 0);
			}
			else{
					std::string err = "-ERR unknown command '" + args[0] + "'\r\n";
					send(client_fd, err.c_str(), err.size(), 0);
			}
	}
	fclose(client_file);
}
int main(int argc, char **argv){
	// Flush after every std::cout / std::cerr
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;
	for(int i = 1; i < argc; i++){ // Persistence options
		if(std::string(argv[i]) == "--dir" && i + 1 < argc){
				dir = argv[i + 1];
		}
		if(std::string(argv[i]) == "--dbfilename" && i + 1 < argc) {
				dbfilename = argv[i + 1];
		}
	}
	const std::string rdb_path = dir + "/" + dbfilename;
	load_rdb_file(rdb_path, db);

	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0) {
	 std::cerr << "Failed to create server socket\n";
	 return 1;
	}

	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		std::cerr << "setsockopt failed\n";
		return 1;
	}

	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(6379);

	if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
		std::cerr << "Failed to bind to port 6379\n";
		return 1;
	}

	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
		std::cerr << "listen failed\n";
		return 1;
	}

	struct sockaddr_in client_addr;
	int client_addr_len = sizeof(client_addr);
	std::cout << "Waiting for a client to connect...\n";

	while(true){ // Main loop to accept client connections
		int client_fd = accept(server_fd, (struct sockaddr*)&client_addr,(socklen_t*) &client_addr_len);
		if(client_fd < 0){
				std::cerr<<"accept failed\n";
				continue;
		}
		std::cout<<"Client connected\n";
		// Launch a thread to handle the client
		std::thread t(handle_client, client_fd);
		t.detach(); // run in background and auto-clean up
}

	close(server_fd);

	return 0;
}
