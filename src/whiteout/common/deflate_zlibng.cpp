// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file deflate_zlibng.cpp
/// @brief zlib-wrapped DEFLATE compress/inflate, backed by zlib-ng.
///
/// Selected when WHITEOUT_USE_ZLIBNG=ON; the built-in codec (common/deflate.cpp
/// + storages/common/inflate_fast_builtin.cpp) is used otherwise. zlib-ng is
/// the single TU-local third-party dependency: <zlib-ng.h> is pulled in here
/// and nowhere else. The public surface
/// (whiteout::zlib_compress / zlib_decompress and
/// storages::common::zlibInflateFast) is byte-for-byte source-compatible, so
/// every consumer (PNG, TIFF, BLTE, MPQ) is unaffected.

#include "deflate.h"

#include "../storages/common/inflate_fast.h" // declaration of zlibInflateFast

#include <zlib-ng.h>

namespace whiteout {

namespace {

constexpr int kDefaultLevel = 6; // zlib-ng default — balanced ratio/speed.

/// Grow-as-needed streaming inflate. Handles unknown output size; when
/// @p expectedSize is correct, the first allocation is exact (single pass).
std::vector<u8> inflateStream(std::span<const u8> data, size_t expectedSize, std::string* err) {
    auto setErr = [&](const char* m) {
        if (err)
            *err = m;
    };

    if (data.size() < 6) {
        setErr("Zlib data too short");
        return {};
    }

    zng_stream zs{};
    if (zng_inflateInit(&zs) != Z_OK) {
        setErr("inflateInit failed");
        return {};
    }

    size_t cap = expectedSize > 0 ? expectedSize : data.size() * 4;
    if (cap < 4096)
        cap = 4096;
    std::vector<u8> out(cap);

    zs.next_in = data.data();
    zs.avail_in = static_cast<uint32_t>(data.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uint32_t>(out.size());

    for (;;) {
        int const rc = zng_inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END)
            break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            // Z_DATA_ERROR (bad header/adler/corrupt), Z_NEED_DICT, Z_MEM_ERROR.
            const char* msg = zs.msg;
            zng_inflateEnd(&zs);
            setErr(msg ? msg : "inflate failed");
            return {};
        }
        if (zs.avail_out == 0) {
            size_t const used = out.size();
            out.resize(out.size() * 2);
            zs.next_out = out.data() + used;
            zs.avail_out = static_cast<uint32_t>(out.size() - used);
        } else if (rc == Z_BUF_ERROR) {
            // Output space remains but no progress => input exhausted mid-stream.
            zng_inflateEnd(&zs);
            setErr("Truncated or incomplete zlib stream");
            return {};
        }
    }

    out.resize(zs.total_out);
    zng_inflateEnd(&zs);
    return out;
}

} // namespace

std::vector<u8> zlib_decompress(std::span<const u8> data, std::string* out_error,
                                size_t expectedSize) {
    return inflateStream(data, expectedSize, out_error);
}

std::vector<u8> zlib_compress(std::span<const u8> data, std::string* out_error) {
    size_t const bound = zng_compressBound(data.size());
    std::vector<u8> out(bound);
    size_t destLen = bound;
    int const rc = zng_compress2(out.data(), &destLen, data.data(), data.size(), kDefaultLevel);
    if (rc != Z_OK) {
        if (out_error)
            *out_error = "zlib compression failed";
        return {};
    }
    out.resize(destLen);
    return out;
}

} // namespace whiteout

namespace whiteout::storages::common {

std::vector<u8> zlibInflateFast(std::span<const u8> src, size_t expectedSize) {
    // BLTE frames carry their decompressed size: decompress in a single pass
    // straight into an exactly-sized buffer.
    if (expectedSize == 0)
        return ::whiteout::zlib_decompress(src, nullptr, 0);
    if (src.size() < 6)
        return {};

    std::vector<u8> out(expectedSize);
    auto written = zlibInflateFastInto(out, src);
    if (!written) {
        // Size hint wrong or stream needs the growable path — fall back.
        return ::whiteout::zlib_decompress(src, nullptr, expectedSize);
    }
    out.resize(*written);
    return out;
}

std::optional<size_t> zlibInflateFastInto(std::span<u8> dst, std::span<const u8> src) {
    if (src.size() < 6)
        return std::nullopt;
    size_t destLen = dst.size();
    if (zng_uncompress(dst.data(), &destLen, src.data(), src.size()) != Z_OK)
        return std::nullopt;
    return destLen;
}

} // namespace whiteout::storages::common
