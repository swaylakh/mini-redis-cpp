#ifndef RDB_HPP
#define RDB_HPP

#include <string>
#include <unordered_map>
#include <chrono>

struct Entry {
    std::string value;
    std::chrono::system_clock::time_point expires_at;
};

void load_rdb_file(const std::string& path, std::unordered_map<std::string, Entry>& db);

#endif
