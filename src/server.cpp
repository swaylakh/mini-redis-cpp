#include <iostream>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <vector>
#include <random>
#include "server.hpp"
#include "command.hpp"
#include "replication.hpp"
#include "rdb.hpp"

// Global definitions (declared extern in server.hpp)
std::string dir, dbfilename;
int server_port = 6379;
std::unordered_map<std::string, Entry> db;
std::unordered_map<int, Client> clients;

std::string master_replid;
int64_t master_repl_offset = 0;
std::string replicaof_host;
int replicaof_port = 0;
int master_fd = -1;

static void set_nonblocking(int fd){
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
