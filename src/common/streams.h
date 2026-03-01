// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <streambuf>
#include <vector>
#include <cstdint>
#include <span>
#include "../include/common/common_types.h"

namespace whiteout {
namespace common {

class span_streambuf : public std::streambuf {
public:
    span_streambuf(std::span<const u8> data) {
        char* begin = const_cast<char*>(
            reinterpret_cast<const char*>(data.data())
        );
        setg(begin, begin, begin + data.size());
    }
};

class vector_streambuf : public std::streambuf {
public:
    explicit vector_streambuf(std::vector<u8>& buffer)
        : buffer_(buffer) {}

protected:
    // Write a single character
    int overflow(int ch) override {
        if (ch == traits_type::eof())
            return traits_type::eof();

        buffer_.push_back(static_cast<u8>(ch));
        return ch;
    }

    // Write multiple characters
    std::streamsize xsputn(const char* s, std::streamsize count) override {
        auto start = reinterpret_cast<const u8*>(s);
        buffer_.insert(buffer_.end(), start, start + count);
        return count;
    }

private:
    std::vector<u8>& buffer_;
};

} // namespace common
} // namespace whiteout