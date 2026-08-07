#ifndef RDB_HPP
#define RDB_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <chrono>

struct Entry {
    std::string value;
    std::chrono::system_clock::time_point expires_at;
};

void load_rdb_file(const std::string& path, std::unordered_map<std::string, Entry>& db);
std::vector<uint8_t> build_rdb(const std::unordered_map<std::string, Entry>& db);
void save_rdb_file(const std::string& path, const std::unordered_map<std::string, Entry>& db);

#endif
