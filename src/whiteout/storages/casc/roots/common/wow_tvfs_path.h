// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file wow_tvfs_path.h
/// @brief Decoder for WoW retail TVFS encoded leaf names.
///
/// Modern WoW retail (build 30080+) stores file metadata directly in the leaf
/// names of its TVFS manifest. Each leaf name looks like one of:
///
///   LLLLLLLLCCCC:IIIIIIIIKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK   (53 chars, build 46144+)
///   LLLLLLLLCCCCCCCC:IIIIIIIIKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK (57 chars, build 63728+)
///                  ^ colon at position 12 or 16
///   L = locale flags (32-bit, 8 hex digits)
///   C = content flags (16-bit / 32-bit, 4 or 8 hex digits)
///   I = FileDataId (32-bit, 8 hex digits)
///   K = CKey (16 bytes, 32 hex digits)
///
/// CascLib (CascRootFile_TVFS.cpp:733) also recognises a buggy 52-char
/// build-45779 form with a missing CKey nibble; we skip it (matches the
/// `#ifdef TVFS_PARSE_WOW_ROOT` branch which only accepts 53/57).
///
/// When the WoW vfs-root references sub-manifests, the surrounding TVFS
/// traversal prefixes each leaf with `<container>:` (and may nest further),
/// so a real path looks like `vfs-1:LLLLLLLLCCCC:IIIIIIIIKKKK...`. This
/// decoder operates on the trailing 53- or 57-char window so any prefix is
/// transparently skipped.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace whiteout::storages::casc::wow_tvfs_path {

/// Decoded WoW TVFS leaf-name fields.
struct Info {
    u32 localeFlags = 0;
    u32 contentFlags = 0;
    u32 fileDataId = 0;
    std::array<u8, 16> cKey{};
};

namespace detail {

inline u8 hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
    return 0xFF;
}

/// Parse @p len hex digits at @p s into a u32 (most-significant nibble first).
/// Returns false if any character isn't hex.
inline bool parseHexU32(const char* s, size_t len, u32& out) noexcept {
    out = 0;
    for (size_t i = 0; i < len; ++i) {
        u8 d = hexDigit(s[i]);
        if (d == 0xFF) return false;
        out = (out << 4) | d;
    }
    return true;
}

/// Parse 32 hex digits at @p s into a 16-byte key. Returns false on any
/// non-hex character.
inline bool parseHexKey(const char* s, std::array<u8, 16>& out) noexcept {
    for (size_t i = 0; i < 16; ++i) {
        u8 hi = hexDigit(s[i * 2]);
        u8 lo = hexDigit(s[i * 2 + 1]);
        if (hi == 0xFF || lo == 0xFF) return false;
        out[i] = static_cast<u8>((hi << 4) | lo);
    }
    return true;
}

/// Try to parse exactly the given window as a `LLL...:III...K...` segment.
/// @p colonPos must be 12 (4-hex content flags) or 16 (8-hex content flags).
/// The window's total length must equal `colonPos + 1 + 40`.
inline bool parseSegment(std::string_view seg, size_t colonPos, Info& info) noexcept {
    const size_t expected = colonPos + 1 + 40;
    if (seg.size() != expected) return false;
    if (seg[colonPos] != ':') return false;

    // Locale flags: always 8 hex digits.
    if (!parseHexU32(seg.data(), 8, info.localeFlags)) return false;

    // Content flags: (colonPos - 8) hex digits, i.e. 4 or 8.
    const size_t contentLen = colonPos - 8;
    if (!parseHexU32(seg.data() + 8, contentLen, info.contentFlags)) return false;

    // After the colon: 8-hex FileDataId + 32-hex CKey.
    if (!parseHexU32(seg.data() + colonPos + 1, 8, info.fileDataId)) return false;
    if (!parseHexKey(seg.data() + colonPos + 9, info.cKey)) return false;
    return true;
}

} // namespace detail

/// Try to decode the WoW-TVFS-encoded suffix of @p path into @p info.
///
/// Accepts both un-prefixed and sub-manifest-prefixed paths. Returns false
/// if the trailing window doesn't match either the 53- or 57-character WoW
/// retail encoding (with a `:` at the corresponding position).
inline bool tryDecode(std::string_view path, Info& info) noexcept {
    // Length-57 form first (build 63728+, 8-hex content flags).
    if (path.size() >= 57) {
        if (detail::parseSegment(path.substr(path.size() - 57), 16, info))
            return true;
    }
    // Length-53 form (build 46144+, 4-hex content flags).
    if (path.size() >= 53) {
        if (detail::parseSegment(path.substr(path.size() - 53), 12, info))
            return true;
    }
    return false;
}

/// Convenience: just check whether the path is a valid WoW TVFS encoded leaf.
inline bool matches(std::string_view path) noexcept {
    Info info;
    return tryDecode(path, info);
}

} // namespace whiteout::storages::casc::wow_tvfs_path
