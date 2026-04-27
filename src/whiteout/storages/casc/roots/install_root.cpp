// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "install_root.h"
#include "common/root_build_utils.h"
#include "../../common/hex.h"
#include "../../common/jenkins.h"

#include <algorithm>
#include <cstring>

namespace whiteout::storages::casc {

using storages::common::jenkinsHash;

// ── Big-endian read helpers ─────────────────────────────────────────────────

namespace {

u16 readBE16(const u8* p) {
    return u16(p[0]) << 8 | u16(p[1]);
}

u32 readBE32(const u8* p) {
    return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | u32(p[3]);
}

/// Read a null-terminated string starting at data[pos].
/// Advances pos past the terminator.  Returns empty on overflow.
std::string_view readCString(const u8* data, size_t size, size_t& pos) {
    if (pos >= size) return {};
    const u8* start = data + pos;
    const u8* end = static_cast<const u8*>(std::memchr(start, 0, size - pos));
    if (!end) return {};
    size_t len = static_cast<size_t>(end - start);
    pos += len + 1; // skip past NUL
    return {reinterpret_cast<const char*>(start), len};
}

/// Reverse bits in a byte (for install manifest bit-reversal of tag masks).
/// Uses the multiplication trick from CascLib:
///   bits[j] = (byte)((bits[j] * 0x0202020202 & 0x010884422010) % 1023)
inline u8 reverseBits(u8 b) {
    return static_cast<u8>((u64(b) * 0x0202020202ULL & 0x010884422010ULL) % 1023);
}

} // anonymous namespace

// ── Parser ──────────────────────────────────────────────────────────────────

std::unique_ptr<InstallRoot> InstallRoot::parse(std::span<const u8> data,
                                                 interfaces::WorkerPool* /*pool*/) {
    // Minimum header size: 2 (magic) + 1 (version) + 1 (hashSize) + 2 (numTags) + 4 (numFiles) = 10
    if (data.size() < 10) return nullptr;

    // Validate magic: 'IN' (0x49, 0x4E).
    if (data[0] != 'I' || data[1] != 'N') return nullptr;

    [[maybe_unused]] u8 version = data[2];
    u8 hashSize = data[3];
    u16 numTags = readBE16(data.data() + 4);
    u32 numFiles = readBE32(data.data() + 6);

    // Sanity: hashSize is typically 16 (MD5).  Allow 16 or 32.
    if (hashSize != 16 && hashSize != 32) return nullptr;

    // Upper bound sanity.
    if (numFiles > 10'000'000) return nullptr;

    size_t pos = 10;
    u32 numMaskBytes = (numFiles + 7) / 8;

    auto root = std::make_unique<InstallRoot>();

    // ── Read tags ───────────────────────────────────────────────────────
    // Each tag: CString name + i16 BE type + numMaskBytes bitarray.
    // We read them but only need names/types for metadata; the bitarrays
    // associate tags with files but we don't use them for file access.
    std::vector<std::vector<u8>> tagBits;
    tagBits.reserve(numTags);
    for (u16 t = 0; t < numTags; ++t) {
        auto name = readCString(data.data(), data.size(), pos);
        if (name.data() == nullptr) return nullptr;
        if (pos + 2 > data.size()) return nullptr;
        i16 type = static_cast<i16>(readBE16(data.data() + pos));
        pos += 2;

        if (pos + numMaskBytes > data.size()) return nullptr;

        // Read and bit-reverse the mask bytes (CascLib compat).
        std::vector<u8> bits(data.data() + pos, data.data() + pos + numMaskBytes);
        for (auto& b : bits) b = reverseBits(b);
        pos += numMaskBytes;

        InstallTag tag;
        tag.name = std::string(name);
        tag.type = type;
        root->m_tags.push_back(std::move(tag));
        tagBits.push_back(std::move(bits));
    }

    // ── Read file entries ───────────────────────────────────────────────
    root->m_entries.reserve(numFiles);
    for (u32 i = 0; i < numFiles; ++i) {
        auto name = readCString(data.data(), data.size(), pos);
        if (name.data() == nullptr) return nullptr;
        if (pos + hashSize + 4 > data.size()) return nullptr;

        RootEntry entry{};

        // CKey: first 16 bytes of the hash (even if hashSize > 16).
        std::memcpy(entry.cKey.data(), data.data() + pos,
                    std::min<size_t>(hashSize, 16));
        pos += hashSize;

        // Size: 4 bytes big-endian.
        entry.fileSize = readBE32(data.data() + pos);
        pos += 4;

        entry.path = std::string(name);
        entry.fileDataId = kInvalidFileDataId;
        entry.localeFlags = 0xFFFFFFFF; // All locales.
        entry.contentFlags = 0;

        // Compute Jenkins hash for path lookup.
        auto hash = jenkinsHash(entry.path);
        entry.fileNameHash = u64(hash.pc) | (u64(hash.pb) << 32);

        // Collect tag names into a locale mask heuristic:
        // If a tag of type 0 (locale) matches, use that locale.
        // For now we keep All — tags are informational.

        root->m_entries.push_back(std::move(entry));
    }

    if (root->m_entries.empty()) return nullptr;

    root->buildIndices();
    return root;
}

// ── Lookup ──────────────────────────────────────────────────────────────────

std::vector<const RootEntry*> InstallRoot::findByPath(const std::string& path) const {
    return findByPathOrHash(path, m_entries, m_byPath, m_byNameHash);
}

std::vector<const RootEntry*> InstallRoot::findByFileDataId(u32 /*fileDataId*/, FileIdHint /*hint*/) const {
    return {}; // Install manifest does not use FileDataId.
}

// ── Index building ──────────────────────────────────────────────────────────

void InstallRoot::buildIndices() {
    buildPathAndHashIndex(m_byPath, m_byNameHash, m_entries);
}

} // namespace whiteout::storages::casc
