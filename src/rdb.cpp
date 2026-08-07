#include "rdb.hpp"
#include <fstream>
#include <iostream>
#include <cstdint>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

std::string read_string(std::ifstream& file);
static int read_length(std::ifstream& file);

// CRC64 lookup table — ECMA-182 polynomial (0xad93d23594c935a9), same as Redis
static uint64_t crc64_table[256];
static bool crc64_table_init = false;

static void init_crc64_table(){
    const uint64_t poly = 0xad93d23594c935a9ULL;
    for (int i = 0; i < 256; i++){
        uint64_t crc = i;
        for (int j = 0; j < 8; j++){
            if (crc & 1)
                crc = (crc >> 1) ^ poly;
            else
                crc >>= 1;
        }
        crc64_table[i] = crc;
    }
    crc64_table_init = true;
}

static uint64_t crc64(const std::vector<uint8_t>& data){
    if (!crc64_table_init) init_crc64_table();
    uint64_t crc = 0;
    for (uint8_t byte : data){
        crc = crc64_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

// Write helpers — append bytes to a buffer
static void write_byte(std::vector<uint8_t>& buf, uint8_t b){
    buf.push_back(b);
}

static void write_bytes(std::vector<uint8_t>& buf, const void* data, size_t len){
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + len);
}

// Write a length using RDB's variable-length encoding
static void write_length(std::vector<uint8_t>& buf, uint32_t len){
    if (len < 64){
        // 00xxxxxx — 6-bit length
        write_byte(buf, (uint8_t)len);
    }
    else if (len < 16384){
        // 01xxxxxx yyyyyyyy — 14-bit length
        write_byte(buf, 0x40 | ((len >> 8) & 0x3F));
        write_byte(buf, len & 0xFF);
    }
    else{
        // 10xxxxxx + 4 bytes big-endian
        write_byte(buf, 0x80);
        write_byte(buf, (len >> 24) & 0xFF);
        write_byte(buf, (len >> 16) & 0xFF);
        write_byte(buf, (len >> 8) & 0xFF);
        write_byte(buf, len & 0xFF);
    }
}

// Write a length-prefixed string
static void write_string(std::vector<uint8_t>& buf, const std::string& s){
    write_length(buf, s.size());
    write_bytes(buf, s.data(), s.size());
}

// Read a length-encoded string from the RDB file
// Top 2 bits of the first byte determine the format
std::string read_string(std::ifstream& file) {
    uint8_t first = file.get();
    uint8_t bits = first >> 6; // isolate top 2 bits
    if(bits == 0b00){ // 6-bit length (0-63)
        int len = first & 0x3F;
        std::string s(len, '\0');
        if (len > 0) file.read(&s[0], len);
        return s;
    }
    else if (bits == 0b01){ // 14-bit length (0-16383)
        uint8_t second = file.get();
        int len = ((first & 0x3F) << 8) | second;
        std::string s(len, '\0');
        if (len > 0) file.read(&s[0], len);
        return s;
    }
    else if (bits == 0b10){ // 32-bit length, big-endian
        uint8_t buf[4];
        file.read(reinterpret_cast<char*>(buf), 4);
        uint32_t len = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
        std::string s(len, '\0');
        if (len > 0) file.read(&s[0], len);
        return s;
    }
    else if (bits == 0b11){ // special: integer stored as string
        uint8_t type = first & 0x3F;
        if (type == 0){ // 8-bit signed int
            int8_t val = file.get();
            return std::to_string(val);
        }
        else if (type == 1){ // 16-bit signed int, little-endian
            int16_t val;
            file.read(reinterpret_cast<char*>(&val), 2);
            return std::to_string(val);
        }
        else if (type == 2){ // 32-bit signed int, little-endian
            int32_t val;
            file.read(reinterpret_cast<char*>(&val), 4);
            return std::to_string(val);
        }
        else{
            throw std::runtime_error("Unsupported encoding type in read_string: " + std::to_string(type));
        }
    }
    throw std::runtime_error("Invalid length prefix in read_string");
}

// Read a length-only value (no special string encoding allowed)
static int read_length(std::ifstream& file) {
    uint8_t first = file.get();
    uint8_t bits = first >> 6;
    if(bits == 0b00){ // 6-bit length
        return first & 0x3F;
    }
    else if (bits == 0b01){ // 14-bit length
        uint8_t second = file.get();
        return ((first & 0x3F) << 8) | second;
    }
    else if (bits == 0b10){ // 32-bit length, big-endian
        uint8_t buf[4];
        file.read(reinterpret_cast<char*>(buf), 4);
        return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    }
    else if (bits == 0b11) { // can't have an integer where we expect a length
        uint8_t encoding_type = first & 0x3F;
        throw std::runtime_error("Encountered special string encoding type in read_length: " + std::to_string(encoding_type));
    }
    return -1;
}


void load_rdb_file(const std::string& path, std::unordered_map<std::string, Entry>& db){
    std::ifstream file(path, std::ios::binary);
    if(!file.is_open()){
        std::cerr << "RDB file not found, continuing with empty DB\n";
        return;
    }

    // Read and ignore header (Redis + Version)
    file.ignore(9);

    // Declare current_entry_expiry outside the loop so its value persists
    // until a key-value pair is processed.
    std::chrono::time_point<std::chrono::system_clock> current_entry_expiry =
        std::chrono::time_point<std::chrono::system_clock>::max();
    while(file.good()){ // Use .good() for robust stream state checking
        int peeked_byte_int = file.peek();
        uint8_t opcode = static_cast<uint8_t>(peeked_byte_int);

        if(opcode == 0xFD){ // EXPIRETIME in seconds (Unix timestamp)
            file.get(); // consume opcode
            uint32_t seconds_since_epoch;
            file.read(reinterpret_cast<char*>(&seconds_since_epoch), 4);
            // Correctly set expiry as an ABSOLUTE time from epoch
            current_entry_expiry = std::chrono::time_point<std::chrono::system_clock>(
                std::chrono::seconds(seconds_since_epoch));
        }
        else if(opcode == 0xFC){ // EXPIRETIME in milliseconds (Unix timestamp)
            file.get(); // consume opcode
            uint64_t ms_since_epoch;
            file.read(reinterpret_cast<char*>(&ms_since_epoch), 8);
            
            // Correctly set expiry as an ABSOLUTE time from epoch
            current_entry_expiry = std::chrono::time_point<std::chrono::system_clock>(
                std::chrono::milliseconds(ms_since_epoch));
        }
        else if (opcode == 0xFA) { // AUX field
            file.get(); // consume opcode
            read_string(file); // key
            read_string(file); // value
        }
        else if (opcode == 0xFE){ // DB selector
            file.get(); // consume opcode
            read_length(file); // DB number is length-encoded
        }
        else if (opcode == 0xFB) { // RESIZEDB opcode
            file.get(); // consume 0xFB
            read_length(file); // Read db_size
            read_length(file); // Read expires_size
        }
        else if(opcode == 0x00){ // string type 
            file.get(); // consume type byte (0x00)
            std::string key = read_string(file);
            std::string val = read_string(file);
            // Store with the calculated system_clock expiry
            db[key] = Entry{val, current_entry_expiry};
            // Reset current_entry_expiry ONLY AFTER it's applied to a key-value pair
            current_entry_expiry = std::chrono::time_point<std::chrono::system_clock>::max();
        }
        else if(opcode == 0xff){ // EOF opcode
            file.get(); // consume 0xff

            // Verify CRC64 checksum if present
            auto data_end = file.tellg(); // position right after 0xFF
            uint64_t stored_crc;
            file.read(reinterpret_cast<char*>(&stored_crc), 8);
            if (file.gcount() == 8){
                // Re-read everything from start up to (including) 0xFF
                file.seekg(0);
                std::vector<uint8_t> raw(data_end);
                file.read(reinterpret_cast<char*>(raw.data()), data_end);
                uint64_t computed_crc = crc64(raw);
                if (computed_crc != stored_crc){
                    std::cerr << "WARNING: CRC64 mismatch — RDB file may be corrupted\n";
                }
            }

            break;
        }
        else {
            std::cerr << "Unknown type or opcode: " << std::hex << (int)opcode << "\n";
            break; // Stop parsing on unknown/unhandled opcode/type
        }
    }
    file.close();
}


// Build the RDB snapshot as a byte vector (reused by SAVE and PSYNC)
std::vector<uint8_t> build_rdb(const std::unordered_map<std::string, Entry>& db){
    std::vector<uint8_t> buf;

    // Header: "REDIS0011" (9 bytes)
    const char* header = "REDIS0011";
    write_bytes(buf, header, 9);

    // AUX fields — metadata about this server
    write_byte(buf, 0xFA); // AUX opcode
    write_string(buf, "redis-ver");
    write_string(buf, "mini-redis");

    write_byte(buf, 0xFA);
    write_string(buf, "redis-bits");
    write_string(buf, "64");

    // DB selector — we only use database 0
    write_byte(buf, 0xFE);
    write_length(buf, 0);

    // RESIZEDB — hint for hash table pre-allocation
    int expires_count = 0;
    for (const auto& [key, entry] : db){
        if (entry.expires_at != std::chrono::system_clock::time_point::max())
            expires_count++;
    }
    write_byte(buf, 0xFB);
    write_length(buf, db.size());
    write_length(buf, expires_count);

    // Key-value pairs
    for (const auto& [key, entry] : db){
        // Write expiry if present
        if (entry.expires_at != std::chrono::system_clock::time_point::max()){
            write_byte(buf, 0xFC); // expiry in milliseconds
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.expires_at.time_since_epoch()).count();
            uint64_t ms_val = static_cast<uint64_t>(ms);
            write_bytes(buf, &ms_val, 8); // little-endian on x86
        }

        write_byte(buf, 0x00); // type = string
        write_string(buf, key);
        write_string(buf, entry.value);
    }

    // EOF marker
    write_byte(buf, 0xFF);

    // CRC64 checksum over everything written so far
    uint64_t checksum = crc64(buf);
    write_bytes(buf, &checksum, 8); // little-endian

    return buf;
}

void save_rdb_file(const std::string& path, const std::unordered_map<std::string, Entry>& db){
    std::vector<uint8_t> buf = build_rdb(db);

    // Write the whole buffer to disk atomically
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()){
        std::cerr << "Failed to open " << path << " for writing\n";
        return;
    }
    file.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    file.close();
}