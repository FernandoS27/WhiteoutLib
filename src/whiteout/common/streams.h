// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <streambuf>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {
namespace common {

class span_streambuf : public std::streambuf {
public:
    span_streambuf(std::span<const u8> data) {
        // setg() requires char* even for input-only streambufs (legacy API).
        // We only ever read via eback()/gptr()/egptr(), so the cast is safe.
        char* begin = const_cast<char*>( // NOLINT(cppcoreguidelines-pro-type-const-cast)
            reinterpret_cast<const char*>(data.data()));
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
    explicit vector_streambuf(std::vector<u8>& buffer) : buffer_(buffer), pos_(buffer.size()) {}

protected:
    // Write a single character
    int overflow(int ch) override {
        if (ch == traits_type::eof())
            return traits_type::not_eof(ch);

        if (pos_ < buffer_.size()) {
            buffer_[pos_] = static_cast<u8>(ch);
        } else {
            buffer_.push_back(static_cast<u8>(ch));
        }
        ++pos_;
        return ch;
    }

    // Write multiple characters
    std::streamsize xsputn(const char* s, std::streamsize count) override {
        auto start = reinterpret_cast<const u8*>(s);
        std::size_t n = static_cast<std::size_t>(count);

        if (pos_ + n <= buffer_.size()) {
            // Overwrite existing data
            std::memcpy(buffer_.data() + pos_, start, n);
        } else if (pos_ < buffer_.size()) {
            // Partial overwrite + append
            std::size_t overwrite = buffer_.size() - pos_;
            std::memcpy(buffer_.data() + pos_, start, overwrite);
            buffer_.insert(buffer_.end(), start + overwrite, start + n);
        } else {
            // Pure append (pos_ may be past end if seeks happened)
            buffer_.insert(buffer_.end(), start, start + n);
        }
        pos_ += n;
        return count;
    }

    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode which = std::ios_base::out) override {
        if (!(which & std::ios_base::out))
            return pos_type(off_type(-1));

        std::size_t newpos;
        switch (dir) {
        case std::ios_base::beg:
            newpos = static_cast<std::size_t>(off);
            break;
        case std::ios_base::cur:
            newpos = pos_ + static_cast<std::size_t>(off);
            break;
        case std::ios_base::end:
            newpos = buffer_.size() + static_cast<std::size_t>(off);
            break;
        default:
            return pos_type(off_type(-1));
        }
        if (newpos > buffer_.size())
            return pos_type(off_type(-1));
        pos_ = newpos;
        return pos_type(static_cast<off_type>(pos_));
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode which = std::ios_base::out) override {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }

private:
    std::vector<u8>& buffer_;
    std::size_t pos_;
};

} // namespace common
} // namespace whiteout