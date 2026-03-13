// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <cstdint>
#include <span>
#include <streambuf>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {
namespace common {

class span_streambuf : public std::streambuf {
public:
    span_streambuf(std::span<const u8> data) {
        char* begin = const_cast<char*>(reinterpret_cast<const char*>(data.data()));
        setg(begin, begin, begin + data.size());
    }

protected:
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode which = std::ios_base::in) override {
        if (!(which & std::ios_base::in))
            return pos_type(off_type(-1));
        char* newpos;
        switch (dir) {
        case std::ios_base::beg:
            newpos = eback() + off;
            break;
        case std::ios_base::cur:
            newpos = gptr() + off;
            break;
        case std::ios_base::end:
            newpos = egptr() + off;
            break;
        default:
            return pos_type(off_type(-1));
        }
        if (newpos < eback() || newpos > egptr())
            return pos_type(off_type(-1));
        setg(eback(), newpos, egptr());
        return pos_type(newpos - eback());
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode which = std::ios_base::in) override {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }
};

class vector_streambuf : public std::streambuf {
public:
    explicit vector_streambuf(std::vector<u8>& buffer) : buffer_(buffer) {}

protected:
    // Write a single character
    int overflow(int ch) override {
        if (ch == traits_type::eof())
            return traits_type::not_eof(ch);

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