// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "sparse.h"

#include <cstring>

namespace whiteout::storages::mpq {

std::vector<u8> sparseDecompress(std::span<const u8> src, size_t expectedSize) {
    // Need at least 5 bytes: 4-byte big-endian original size + at least 1 token.
    if (src.size() < 5)
        return {};

    // Read the 4-byte big-endian original size.
    u32 const origSize = (static_cast<u32>(src[0]) << 24) | (static_cast<u32>(src[1]) << 16) |
                         (static_cast<u32>(src[2]) << 8) | (static_cast<u32>(src[3]) << 0);

    if (origSize > expectedSize)
        return {};

    std::vector<u8> out;
    out.reserve(origSize);

    size_t inPos = 4;

    while (inPos < src.size() && out.size() < origSize) {
        u8 const token = src[inPos++];

        if (token & 0x80) {
            // Literal run: (token & 0x7F) + 1 literal bytes follow.
            size_t const count = (token & 0x7F) + 1;
            if (inPos + count > src.size())
                return {};
            size_t const room = origSize - out.size();
            size_t const toCopy = count < room ? count : room;
            out.insert(out.end(), src.data() + inPos, src.data() + inPos + toCopy);
            inPos += count;
        } else {
            // Zero run: (token & 0x7F) + 3 zero bytes.
            size_t const count = (token & 0x7F) + 3;
            size_t const room = origSize - out.size();
            size_t const toFill = count < room ? count : room;
            out.resize(out.size() + toFill, 0);
        }
    }

    return out;
}

std::vector<u8> sparseCompress(std::span<const u8> src) {
    if (src.empty())
        return {};

    std::vector<u8> out;
    out.reserve(4 + src.size());

    // 4-byte big-endian original size.
    u32 const origSize = static_cast<u32>(src.size());
    out.push_back(static_cast<u8>(origSize >> 24));
    out.push_back(static_cast<u8>(origSize >> 16));
    out.push_back(static_cast<u8>(origSize >> 8));
    out.push_back(static_cast<u8>(origSize >> 0));

    size_t pos = 0;
    while (pos < src.size()) {
        // Count consecutive zero bytes.
        size_t const zeroStart = pos;
        while (pos < src.size() && src[pos] == 0 && (pos - zeroStart) < (0x7F + 3)) {
            pos++;
        }
        size_t const zeroCount = pos - zeroStart;

        if (zeroCount >= 3) {
            // Emit zero-run token: (count - 3) with high bit clear.
            out.push_back(static_cast<u8>(zeroCount - 3));
        } else {
            // Rewind — these zeros will be emitted as literals.
            pos = zeroStart;

            // Count literal bytes (non-zero, or short zero runs < 3).
            size_t const litStart = pos;
            while (pos < src.size() && (pos - litStart) < (0x7F + 1)) {
                // Look ahead: if 3+ zeros, end the literal run.
                if (src[pos] == 0) {
                    size_t ahead = pos;
                    while (ahead < src.size() && src[ahead] == 0 && (ahead - pos) < 3)
                        ahead++;
                    if (ahead - pos >= 3)
                        break;
                }
                pos++;
            }
            size_t litCount = pos - litStart;
            if (litCount == 0) {
                // Edge case: we're at zeros but fewer than 3. Emit as literal.
                litCount = 1;
                pos = litStart + 1;
            }
            // Emit literal token: 0x80 | (count - 1).
            out.push_back(static_cast<u8>(0x80 | (litCount - 1)));
            out.insert(out.end(), src.data() + litStart, src.data() + litStart + litCount);
        }
    }

    return out;
}

} // namespace whiteout::storages::mpq
