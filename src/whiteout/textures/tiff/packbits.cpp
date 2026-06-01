// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "packbits.h"

namespace whiteout::textures::tiff {

std::vector<u8> packBitsDecompress(std::span<const u8> src, size_t expectedSize) {
    std::vector<u8> out;
    if (expectedSize > 0)
        out.reserve(expectedSize);

    size_t i = 0;
    while (i < src.size()) {
        const i8 header = static_cast<i8>(src[i++]);
        if (header == -128) {
            // No-op.
            continue;
        }
        if (header >= 0) {
            // Literal run of header+1 bytes.
            const size_t n = static_cast<size_t>(header) + 1;
            if (i + n > src.size())
                return {};
            out.insert(out.end(), src.begin() + i, src.begin() + i + n);
            i += n;
        } else {
            // Run of (-header + 1) copies of the next byte.
            const size_t n = static_cast<size_t>(-static_cast<int>(header)) + 1;
            if (i >= src.size())
                return {};
            const u8 v = src[i++];
            out.insert(out.end(), n, v);
        }
        if (expectedSize > 0 && out.size() > expectedSize) {
            // Overshoot — malformed.
            return {};
        }
    }
    return out;
}

} // namespace whiteout::textures::tiff
