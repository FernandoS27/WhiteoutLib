#pragma once

#include "common_types.h"
#include "concepts.h"
#include <ostream>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <string>

namespace common {

class BinaryWriter {
public:
    BinaryWriter(std::ostream& outputStream) : file(outputStream) {
        // No need to open the stream, as it is managed externally
    }
    
    ~BinaryWriter() {
        // Do not close the stream, as it is managed externally
    }
    
    // Write basic types
    template<BinaryBlob T>
    void write(const T& value) {
        file.write(
            reinterpret_cast<const char*>(std::addressof(value)),
            sizeof(T)
        );
    }

    // Write collections
    template<TrivialContiguousRange C>
    void write(const C& c) {
        using T = std::remove_cv_t<
            std::remove_pointer_t<decltype(std::data(c))>
        >;

        file.write(
            reinterpret_cast<const char*>(std::data(c)),
            std::size(c) * sizeof(T)
        );
    }

    void writeBytes(const char* buffer, u32 count) {
        file.write(buffer, count);
    }
    
    // Write string with fixed size (padded with zeros)
    void writeString(const std::string& str, u32 size) {
        std::vector<char> buffer(size, '\0');
        size_t copySize = std::min(static_cast<size_t>(size), str.size());
        std::memcpy(buffer.data(), str.c_str(), copySize);
        file.write(buffer.data(), size);
    }

    void writeString(const std::string& str) {
        file.write(str.c_str(), str.size());
    }
    
    // Write variable length string (null-terminated)
    void writeZString(const std::string& str) {
        file.write(str.c_str(), str.size());
        write<u8>(0); // null terminator
    }
    
    // Get current position
    u32 getPosition() {
        return file.tellp();
    }
    
    // Set position
    void setPosition(u32 pos) {
        file.seekp(pos, std::ios::beg);
    }
    
    // Write padding
    void writePadding(u32 count) {
        for (u32 i = 0; i < count; i++) {
            write<u8>(0U);
        }
    }

    void AlignTo(u32 alignment) {
        u32 currentPos = getPosition();
        u32 padding = (alignment - (currentPos % alignment)) % alignment;
        writePadding(padding);
    }
    
    // Check if stream is valid
    bool isValid() const {
        return file.good();
    }
    
private:
    std::ostream& file;
};

} // namespace common
