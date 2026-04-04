// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "s1_root.h"
#include "common/root_build_utils.h"
#include "../../common/hex.h"
#include "../../common/jenkins.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace whiteout::storages::casc {

using storages::common::hexDecode16;
using storages::common::jenkinsHash;

// ── Locale flag mapping ─────────────────────────────────────────────────────

namespace {

/// Map locale name strings (from the S1 root "path:locale|ckey" format) to
/// the standard Blizzard locale bitmask values.  S1 uses the same locale
/// system as WoW/CASC.
u32 parseLocaleFlags(std::string_view name) {
    // Standard CASC locale flags (matches Blizzard's LocaleFlags enum).
    if (name == "All")   return 0xFFFFFFFF;
    if (name == "enUS")  return 0x2;
    if (name == "koKR")  return 0x4;
    if (name == "frFR")  return 0x10;
    if (name == "deDE")  return 0x20;
    if (name == "zhCN")  return 0x40;
    if (name == "esES")  return 0x80;
    if (name == "zhTW")  return 0x100;
    if (name == "enGB")  return 0x200;
    if (name == "enCN")  return 0x400;
    if (name == "enTW")  return 0x800;
    if (name == "esMX")  return 0x1000;
    if (name == "ruRU")  return 0x2000;
    if (name == "ptBR")  return 0x4000;
    if (name == "itIT")  return 0x8000;
    if (name == "ptPT")  return 0x10000;
    if (name == "plPL")  return 0x20000;
    if (name == "jaJP")  return 0x40000;
    if (name == "thTH")  return 0x80000;
    // Unknown locale → treat as All.
    return 0xFFFFFFFF;
}

} // anonymous namespace

// ── Heuristic detection ─────────────────────────────────────────────────────

bool S1Root::looksLikeS1Root(std::span<const u8> data) {
    if (data.empty()) return false;

    // Must not start with '#' (that's Overwatch).
    if (data[0] == '#') return false;

    // Must start with a printable ASCII character (file path).
    if (data[0] < 0x20 || data[0] > 0x7E) return false;

    // Find the first pipe and first newline in the first 512 bytes.
    size_t limit = std::min<size_t>(data.size(), 512);
    size_t pipePos = 0, newlinePos = 0;
    bool foundPipe = false, foundNewline = false;
    for (size_t i = 0; i < limit; ++i) {
        if (!foundPipe && data[i] == '|') { pipePos = i; foundPipe = true; }
        if (!foundNewline && (data[i] == '\n' || data[i] == '\r')) {
            newlinePos = i; foundNewline = true;
        }
        if (foundPipe && foundNewline) break;
    }

    if (!foundPipe) return false;

    // The part after the pipe should be a hex string (32 chars for MD5).
    // Check that the characters after the pipe are hex digits.
    size_t hexStart = pipePos + 1;
    size_t hexEnd = foundNewline ? newlinePos : limit;
    size_t hexLen = 0;
    for (size_t i = hexStart; i < hexEnd; ++i) {
        char c = static_cast<char>(data[i]);
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            ++hexLen;
        else
            break;
    }

    // Expect exactly 32 hex characters (16-byte MD5).
    return hexLen == 32;
}

// ── Parser ──────────────────────────────────────────────────────────────────

std::unique_ptr<S1Root> S1Root::parse(std::span<const u8> data,
                                       interfaces::WorkerPool* /*pool*/) {
    if (data.empty()) return nullptr;

    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());

    // Skip UTF-8 BOM if present.
    if (text.size() >= 3 && text[0] == '\xEF' && text[1] == '\xBB' && text[2] == '\xBF')
        text.remove_prefix(3);

    auto root = std::make_unique<S1Root>();

    // Parse line by line.
    size_t pos = 0;
    while (pos < text.size()) {
        // Find end of line.
        size_t eol = text.find_first_of("\r\n", pos);
        if (eol == std::string_view::npos) eol = text.size();

        std::string_view line = text.substr(pos, eol - pos);

        // Advance past newline(s).
        pos = eol;
        if (pos < text.size() && text[pos] == '\r') ++pos;
        if (pos < text.size() && text[pos] == '\n') ++pos;

        // Skip empty lines and comments.
        if (line.empty() || line[0] == '#') continue;

        // Format: filepath|ckey_hex  or  filepath:locale|ckey_hex
        size_t pipe = line.find('|');
        if (pipe == std::string_view::npos || pipe == 0) continue;

        std::string_view pathPart = line.substr(0, pipe);
        std::string_view ckeyHex = line.substr(pipe + 1);

        // Trim trailing whitespace from ckey hex.
        while (!ckeyHex.empty() && (ckeyHex.back() == ' ' || ckeyHex.back() == '\t'))
            ckeyHex.remove_suffix(1);

        // Parse CKey.
        if (ckeyHex.size() < 32) continue; // Need at least 32 hex chars.
        auto cKey = hexDecode16(ckeyHex.substr(0, 32));

        // Check for locale suffix: "filepath:locale"
        std::string filePath;
        u32 localeFlags = 0xFFFFFFFF; // All by default.
        size_t colon = pathPart.find(':');
        if (colon != std::string_view::npos) {
            filePath = std::string(pathPart.substr(0, colon));
            localeFlags = parseLocaleFlags(pathPart.substr(colon + 1));
        } else {
            filePath = std::string(pathPart);
        }

        // Compute Jenkins hash for path lookup.
        auto hash = jenkinsHash(filePath);
        u64 combinedHash = u64(hash.pc) | (u64(hash.pb) << 32);

        RootEntry entry{};
        entry.cKey = cKey;
        entry.fileNameHash = combinedHash;
        entry.localeFlags = localeFlags;
        entry.contentFlags = 0;
        entry.fileDataId = kInvalidFileDataId;
        entry.path = std::move(filePath);

        root->m_entries.push_back(std::move(entry));
    }

    if (root->m_entries.empty()) return nullptr;

    root->buildIndices();
    return root;
}

// ── Lookup ──────────────────────────────────────────────────────────────────

std::vector<const RootEntry*> S1Root::findByPath(const std::string& path) const {
    return findByPathOrHash(path, m_entries, m_byPath, m_byNameHash);
}

std::vector<const RootEntry*> S1Root::findByFileDataId(u32 /*fileDataId*/, FileIdHint /*hint*/) const {
    return {}; // S1 root does not use FileDataId.
}

// ── Index building ──────────────────────────────────────────────────────────

void S1Root::buildIndices() {
    buildPathAndHashIndex(m_byPath, m_byNameHash, m_entries);
}

} // namespace whiteout::storages::casc
