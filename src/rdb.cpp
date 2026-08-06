#include "rdb.hpp"
#include <fstream>
#include <iostream>
#include <cstdint>
#include <chrono>
#include <stdexcept>
#include <string>   
#include <unordered_map>

std::string read_string(std::ifstream& file);
static int read_length(std::ifstream& file);

std::string read_string(std::ifstream& file) {
    uint8_t first = file.get();
    uint8_t bits = first >> 6;
    if(bits == 0b00){
        int len = first & 0x3F;
        std::string s(len, '\0');
        if (len > 0) file.read(&s[0], len);
        return s;
    } 
    else if (bits == 0b01){
        uint8_t second = file.get();
        int len = ((first & 0x3F) << 8) | second;
        std::string s(len, '\0');
        if (len > 0) file.read(&s[0], len);
        return s;
    } 
    else if (bits == 0b10){
        uint8_t buf[4];
        file.read(reinterpret_cast<char*>(buf), 4);
        uint32_t len = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
        std::string s(len, '\0');
        if (len > 0) file.read(&s[0], len);
        return s;
    }
    else if (bits == 0b11){
        uint8_t type = first & 0x3F;
        if (type == 0){ // 8-bit signed int
            int8_t val = file.get();
            return std::to_string(val);
        } 
        else if (type == 1){ // 16-bit signed int
            int16_t val;
            file.read(reinterpret_cast<char*>(&val), 2);
            return std::to_string(val);
        } 
        else if (type == 2){ // 32-bit signed int
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


static int read_length(std::ifstream& file) {
    uint8_t first = file.get();
    uint8_t bits = first >> 6;
    if(bits == 0b00){
        return first & 0x3F;
    }
    else if (bits == 0b01){
        uint8_t second = file.get();
        return ((first & 0x3F) << 8) | second;
    }
    else if (bits == 0b10){
        uint8_t buf[4];
        file.read(reinterpret_cast<char*>(buf), 4);
        return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    }
    else if (bits == 0b11) {
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
            break; // Exit loop cleanly
        }
        else {
            std::cerr << "Unknown type or opcode: " << std::hex << (int)opcode << "\n";
            break; // Stop parsing on unknown/unhandled opcode/type
        }
    }
    file.close();
}