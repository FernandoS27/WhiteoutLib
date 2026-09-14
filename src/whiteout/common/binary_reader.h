// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <array>
#include <cstring>
#include <istream>
#include <memory>
#include <string>
#include <vector>
#include <whiteout/common_types.h>
#include "concepts.h"

namespace whiteout {
namespace common {

class BinaryReader {
public:
    BinaryReader(std::istream& inputStream) : file(inputStream) {
        file.seekg(0, std::ios::end);
        fileSize = static_cast<u32>(file.tellg());
        file.seekg(0, std::ios::beg);
    }

    explicit BinaryReader(std::streambuf& buf)
        : ownedStream_(std::make_unique<std::istream>(&buf)), file(*ownedStream_) {
        file.seekg(0, std::ios::end);
        fileSize = static_cast<u32>(file.tellg());
        file.seekg(0, std::ios::beg);
    }

    ~BinaryReader() {
        // Do not close the stream, as it is managed externally
    }

    template <BinaryBlob T>
    T read() {
        T value;
        file.read(reinterpret_cast<char*>(&value), sizeof(T));
        return value;
    }

    template <TrivialContiguousRange C>
    C read(std::size_t count) {
        C container(count); // works for std::vector
        file.read(reinterpret_cast<char*>(std::data(container)),
                  std::size(container) * sizeof(typename C::value_type));
        return container;
    }

    template <typename T, std::size_t N>
    std::array<T, N> readArray() {
        std::array<T, N> arr;
        file.read(reinterpret_cast<char*>(arr.data()), sizeof(arr));
        return arr;
    }

    void readBytes(char* buffer, u32 count) {
        file.read(buffer, count);
    }

    // Read string with fixed size
    std::string readString(std::size_t size, bool trimNulls = true) {
        std::string str(size, '\0');
        file.read(str.data(), size);
        if (trimNulls) {
            auto nullPos = str.find('\0');
            if (nullPos != std::string::npos)
                str.resize(nullPos);
        }
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
        return static_cast<u32>(file.tellg());
    }

    // Set position
    void setPosition(u32 pos) {
        file.seekg(pos, std::ios::beg);
    }

    // Skip bytes
    void skip(u32 count) {
        file.seekg(count, std::ios::cur);
    }

    // True once a read has run past the end (or otherwise failed). A failed
    // istream serves every later read as a no-op and reports tellg() == -1, so
    // a caller measuring its own progress from getPosition() sees no progress
    // at all -- which is how a chunk walk that overshoots turns into a spin
    // rather than an error. Ask this instead of inferring it from positions.
    bool failed() const {
        return file.fail() || file.tellg() < 0;
    }

    // Check if we have remaining data. A failed stream has none: tellg() is
    // then -1, and comparing that against the size would say "more to read"
    // forever.
    bool hasRemaining() {
        const std::streampos at = file.tellg();
        return at >= 0 && static_cast<u64>(at) < fileSize;
    }

    // Get remaining bytes
    u32 getRemainingBytes() {
        const std::streampos at = file.tellg();
        if (at < 0 || static_cast<u64>(at) >= fileSize)
            return 0;
        return fileSize - static_cast<u32>(at);
    }

    // Check if stream is valid
    bool isValid() const {
        return file.good();
    }

private:
    std::unique_ptr<std::istream> ownedStream_;
    std::istream& file;
    u32 fileSize = 0;
};

} // namespace common
} // namespace whiteout
