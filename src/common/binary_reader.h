// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "../../include/common/common_types.h"
#include "concepts.h"
#include <istream>
#include <cstring>
#include <vector>
#include <string>

namespace whiteout {
namespace common {

class BinaryReader {
public:
    BinaryReader(std::istream& inputStream) : file(inputStream) {
        file.seekg(0, std::ios::end);
        fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
    }
    
    ~BinaryReader() {
        // Do not close the stream, as it is managed externally
    }

    template<BinaryBlob T>
    T read() {
        T value;
        file.read(reinterpret_cast<char*>(&value), sizeof(T));
        return value;
    }

    template<TrivialContiguousRange C>
    C read(std::size_t count) {
        C container(count);  // works for std::vector
        file.read(
            reinterpret_cast<char*>(std::data(container)),
            std::size(container) * sizeof(typename C::value_type)
        );
        return container;
    }

    template<BinaryBlob T, std::size_t N>
    std::array<T, N> readArray() {
        std::array<T, N> arr;
        file.read(reinterpret_cast<char*>(arr.data()), sizeof(arr));
        return arr;
    }
    
    void readBytes(char* buffer, u32 count) {
        file.read(buffer, count);
    }
    
    // Read string with fixed size
    std::string readString(std::size_t size) {
        std::string str(size, '\0');
        file.read(str.data(), size);
        auto nullPos = str.find('\0');
        if (nullPos != std::string::npos) str.resize(nullPos);
        return str;
    }

    std::string readZString() {
        std::string result;
        char c;
        while (file.get(c) && c != '\0') {
            result += c;
        }
        return result;
    }
    
    // Get current position
    u32 getPosition() const {
        return file.tellg();
    }
    
    // Set position
    void setPosition(u32 pos) {
        file.seekg(pos, std::ios::beg);
    }
    
    // Skip bytes
    void skip(u32 count) {
        file.seekg(count, std::ios::cur);
    }
    
    // Check if we have remaining data
    bool hasRemaining() {
        return file.tellg() < fileSize;
    }
    
    // Get remaining bytes
    u32 getRemainingBytes() {
        u32 current = file.tellg();
        return fileSize - current;
    }
    
    // Check if stream is valid
    bool isValid() const {
        return file.good();
    }
    
private:
    std::istream& file;
    u32 fileSize = 0;
};

} // namespace common
} // namespace whiteout
