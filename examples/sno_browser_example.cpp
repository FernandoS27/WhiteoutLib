// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Interactive SNO browser example: opens a CASC storage, loads the CoreTOC,
/// and presents a menu to open SNO files by name or by numeric SNO ID.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/sno/core_toc.h>
#include <whiteout/sno/sno_reader.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>

// ============================================================================
// Combined meta file support
// ============================================================================

/// Signature of D4 combined meta files (e.g. Texture-Global-Global.dat).
static constexpr whiteout::u32 kCombinedMetaMagic = 0x44CF00F5;

/// Cache of SNO entries extracted from combined meta files.
/// Some D4 SNO groups (notably Texture and StringList) store their meta data
/// in combined files rather than individual per-asset files.  The combined
/// file bundles all entries for a group into one blob.
struct CombinedMetaCache {
    /// Index entry: where within a combined file a particular SNO lives.
    struct Entry {
        std::shared_ptr<std::vector<whiteout::u8>> fileData;
        size_t dataOffset;
        size_t dataSize;
        whiteout::sno::SnoGroup group;
    };

    /// snoId -> combined-meta entry.
    std::unordered_map<whiteout::i32, Entry> entries;
};

/// Parse a single combined meta file and add its entries to the cache.
/// @param data      Raw bytes of the combined meta file.
/// @param group     The SNO group this file belongs to.
/// @param cache     Output cache to populate.
static bool parseCombinedMetaFile(
    std::shared_ptr<std::vector<whiteout::u8>> data,
    whiteout::sno::SnoGroup group,
    CombinedMetaCache& cache) {
    using namespace whiteout;

    const auto& buf = *data;
    if (buf.size() < 8)
        return false;

    u32 sig = 0;
    std::memcpy(&sig, buf.data(), 4);
    if (sig != kCombinedMetaMagic)
        return false;

    u32 fileCount = 0;
    std::memcpy(&fileCount, buf.data() + 4, 4);

    // Sanity-check: header must at least contain the index.
    size_t indexEnd = 8 + static_cast<size_t>(fileCount) * 8;
    if (indexEnd > buf.size())
        return false;

    // Read the per-entry index: (snoId: i32, size: u32) pairs.
    struct IndexEntry {
        i32 snoId;
        u32 size;
    };
    std::vector<IndexEntry> index(fileCount);
    for (u32 i = 0; i < fileCount; ++i) {
        size_t off = 8 + static_cast<size_t>(i) * 8;
        std::memcpy(&index[i].snoId, buf.data() + off, 4);
        std::memcpy(&index[i].size, buf.data() + off + 4, 4);
    }

    // Walk the data section.  Each entry is 8-byte aligned, and textures
    // (group 44) have an extra 8-byte gap before each entry.
    const bool isTexture = (group == sno::SnoGroup::Texture);
    constexpr size_t alignment = 8;
    size_t pos = indexEnd;

    for (u32 i = 0; i < fileCount; ++i) {
        // Align to 8 bytes.
        pos = (pos + alignment - 1) & ~(alignment - 1);

        // Textures have an extra 8-byte skip.
        if (isTexture)
            pos += 8;

        if (pos + index[i].size > buf.size())
            break; // truncated file

        // Verify that the first 4 bytes at `pos` match the expected snoId.
        i32 check = 0;
        std::memcpy(&check, buf.data() + pos, 4);
        if (check != index[i].snoId) {
            // Alignment mismatch — skip this entry.
            pos += index[i].size;
            continue;
        }

        CombinedMetaCache::Entry entry;
        entry.fileData   = data;
        entry.dataOffset = pos;
        entry.dataSize   = index[i].size;
        entry.group      = group;

        cache.entries[index[i].snoId] = entry;

        pos += index[i].size;
    }

    return true;
}

/// Try to resolve the SnoGroup from a combined meta file name.
/// Combined meta files follow the naming pattern:
///   {GroupName}-{Category}-{Language}[-{extra}].dat
/// e.g. "Texture-Global-Global.dat", "StringList-Text-enUS.dat".
static whiteout::sno::SnoGroup groupFromCombinedFileName(
    const std::string& fileName) {
    using namespace whiteout::sno;

    // Extract just the file name (strip any path prefix).
    // Handle both regular path separators and CASC virtual prefixes (e.g. "base:").
    std::string name = fileName;
    auto sep = name.find_last_of("\\/:"); // also strip "base:" etc.
    if (sep != std::string::npos)
        name = name.substr(sep + 1);

    // The group name is everything before the first dash.
    auto dash = name.find('-');
    if (dash == std::string::npos)
        return SnoGroup::None;
    std::string groupStr = name.substr(0, dash);

    // Match against known group names.
    for (int gid = -1; gid <= 180; ++gid) {
        auto g = static_cast<SnoGroup>(gid);
        const char* gname = snoGroupName(g);
        if (gname && groupStr == gname)
            return g;
    }
    return SnoGroup::None;
}

/// Enumerate CASC for combined meta files and load them.
static void loadCombinedMetas(
    const whiteout::storages::casc::Storage& storage,
    CombinedMetaCache& cache) {
    using namespace whiteout;

    // Collect candidate combined meta file paths from CASC.
    // Combined metas follow the naming pattern:
    //   {GroupName}-{Category}-{Language}.dat
    // e.g. "Texture-Base-Global.dat", "StringList-Text-enUS.dat"
    // Files with a hex-hash suffix (e.g. "-0x1234abcd.dat") are encrypted
    // differential patches that CascLib cannot read without TACT keys, so
    // we skip them.
    std::vector<std::string> combinedFiles;

    // Enumerate all .dat files from CASC.
    std::vector<std::string> allDatFiles;
    storage.enumerate([&](const whiteout::storages::casc::EnumerateEntry& fe) -> bool {
        if (fe.path.size() >= 4 &&
            fe.path.compare(fe.path.size() - 4, 4, ".dat") == 0)
            allDatFiles.push_back(std::string(fe.path));
        return true;
    });

    // Filter for combined meta files from groups known to use them.
    for (auto& name : allDatFiles) {
        // Extract file name portion (strip path/CASC prefixes).
        std::string fname = name;
        auto sep = fname.find_last_of("\\/:");
        if (sep != std::string::npos)
            fname = fname.substr(sep + 1);

        // Must have at least 2 dashes (Group-Category-Language).
        int dashes = 0;
        for (char c : fname)
            if (c == '-') ++dashes;
        if (dashes < 2)
            continue;

        // Skip encrypted patch variants: files ending in -0x<hex>.dat
        // These have a TACT key hash suffix and require decryption keys.
        {
            auto dot = fname.rfind('.');
            std::string base = (dot != std::string::npos) ? fname.substr(0, dot) : fname;
            auto lastDash = base.rfind('-');
            if (lastDash != std::string::npos) {
                std::string suffix = base.substr(lastDash + 1);
                if (suffix.size() > 2 && suffix[0] == '0' && suffix[1] == 'x')
                    continue;
            }
        }

        // Must resolve to a known SNO group.
        auto group = groupFromCombinedFileName(name);
        if (group != sno::SnoGroup::None)
            combinedFiles.push_back(name);
    }

    if (combinedFiles.empty()) {
        std::cout << "  (no combined meta files found)\n";
        return;
    }

    int loaded = 0;
    for (auto& path : combinedFiles) {
        auto fileData = storage.readFile(path);
        if (!fileData || fileData->empty())
            continue;

        auto group = groupFromCombinedFileName(path);
        if (group == sno::SnoGroup::None)
            continue;

        auto shared = std::make_shared<std::vector<u8>>(std::move(*fileData));
        if (parseCombinedMetaFile(shared, group, cache)) {
            const char* gname = sno::snoGroupName(group);
            std::cout << "  Loaded combined meta: " << path
                      << " (" << (gname ? gname : "?") << ", "
                      << shared->size() << " bytes)\n";
            ++loaded;
        }
    }

    if (loaded > 0)
        std::cout << "  Combined meta cache: " << cache.entries.size()
                  << " entries from " << loaded << " file(s).\n";
    else
        std::cout << "  (found " << combinedFiles.size()
                  << " candidate files but none parsed as combined metas)\n";
}

/// Build a synthetic SNO file (with DEADBEEF header) from combined meta data.
/// Combined meta entries lack the 16-byte SNO header, so we prepend one.
static std::vector<whiteout::u8> buildSnoFromCombinedEntry(
    const CombinedMetaCache::Entry& entry,
    whiteout::u32 formatHash) {
    using namespace whiteout;

    // The final buffer: 16-byte header + entry data.
    std::vector<u8> result(16 + entry.dataSize);

    // Write synthetic header.
    u32 magic = sno::kSnoMagic; // 0xDEADBEEF
    u32 unk08 = 0;
    u32 unk0C = 0;
    std::memcpy(result.data() + 0, &magic, 4);
    std::memcpy(result.data() + 4, &formatHash, 4);
    std::memcpy(result.data() + 8, &unk08, 4);
    std::memcpy(result.data() + 12, &unk0C, 4);

    // Copy entry data (starts with snoId).
    std::memcpy(result.data() + 16,
                entry.fileData->data() + entry.dataOffset,
                entry.dataSize);

    return result;
}

// ============================================================================
// Pretty-print helpers
// ============================================================================

static void indent(int depth) {
    for (int i = 0; i < depth; ++i)
        std::cout << "  ";
}

static void printValue(const whiteout::sno::SnoValue& v, int depth = 0,
                       int maxDepth = 4);

/// Print a single element from a typed array to std::cout.
static void printTypedArrayElem(const whiteout::sno::SnoArray& ta, size_t i) {
    using namespace whiteout::sno;
    switch (ta.kind()) {
    case SVT_BYTE:   std::cout << static_cast<int>(ta.asByteData()[i]); break;
    case SVT_WORD:   std::cout << ta.asWordData()[i]; break;
    case SVT_INT:    std::cout << ta.asIntData()[i]; break;
    case SVT_UINT:   std::cout << ta.asUintData()[i]; break;
    case SVT_FLOAT:  std::cout << ta.asFloatData()[i]; break;
    case SVT_VEC2:   { auto& e = ta.asVec2Data()[i]; std::cout << "(" << e.x << ", " << e.y << ")"; break; }
    case SVT_VEC3:   { auto& e = ta.asVec3Data()[i]; std::cout << "(" << e.x << ", " << e.y << ", " << e.z << ")"; break; }
    case SVT_VEC4:   { auto& e = ta.asVec4Data()[i]; std::cout << "(" << e.x << ", " << e.y << ", " << e.z << ", " << e.w << ")"; break; }
    case SVT_IVEC2:  { auto& e = ta.asIVec2Data()[i]; std::cout << "(" << e.x << ", " << e.y << ")"; break; }
    case SVT_COLOR:  { auto& c = ta.asColorData()[i]; std::cout << "rgba(" << (int)c.r << "," << (int)c.g << "," << (int)c.b << "," << (int)c.a << ")"; break; }
    case SVT_COLORF: { auto& c = ta.asColorFData()[i]; std::cout << "rgbaf(" << c.r << "," << c.g << "," << c.b << "," << c.a << ")"; break; }
    case SVT_REF:    { auto& e = ta.asRefData()[i]; std::cout << "sno_ref(group=" << e.group << ",id=" << e.snoId << ")"; break; }
    case SVT_GBID:   { auto& e = ta.asGbidData()[i]; std::cout << "gbid(group=" << e.group << ",hash=0x" << std::hex << e.raw << std::dec << ")"; break; }
    case SVT_ARRAY:
    case SVT_OBJECT: { printValue(ta.asValueData()[i], 0); break; }
    default: std::cout << "?"; break;
    }
}

static void printValue(const whiteout::sno::SnoValue& v, int depth,
                       int maxDepth) {
    using namespace whiteout::sno;

    if (depth > maxDepth) {
        std::cout << "...";
        return;
    }

    if (v.isNull()) {
        std::cout << "null";
    } else if (v.isBool()) {
        std::cout << (v.asBool() ? "true" : "false");
    } else if (v.isInt()) {
        std::cout << v.asInt();
    } else if (v.isUint()) {
        std::cout << "0x" << std::hex << v.asUint() << std::dec;
    } else if (v.isFloat()) {
        std::cout << v.asFloat();
    } else if (v.isInt64()) {
        std::cout << v.asInt64();
    } else if (v.isUint64()) {
        std::cout << "0x" << std::hex << v.asUint64() << std::dec;
    } else if (v.isByte()) {
        std::cout << static_cast<int>(v.asByte());
    } else if (v.isWord()) {
        std::cout << v.asWord();
    } else if (v.isString()) {
        std::cout << '"' << v.asString() << '"';
    } else if (v.isVec2()) {
        auto& vec = v.asVec2();
        std::cout << "vec2(" << vec.x << ", " << vec.y << ")";
    } else if (v.isVec3()) {
        auto& vec = v.asVec3();
        std::cout << "vec3(" << vec.x << ", " << vec.y << ", " << vec.z << ")";
    } else if (v.isVec4()) {
        auto& vec = v.asVec4();
        std::cout << "vec4(" << vec.x << ", " << vec.y << ", " << vec.z
                  << ", " << vec.w << ")";
    } else if (v.isIVec2()) {
        auto& vec = v.asIVec2();
        std::cout << "ivec2(" << vec.x << ", " << vec.y << ")";
    } else if (v.isColor()) {
        auto& c = v.asColor();
        std::cout << "rgba(" << (int)c.r << ", " << (int)c.g << ", "
                  << (int)c.b << ", " << (int)c.a << ")";
    } else if (v.isColorF()) {
        auto& c = v.asColorF();
        std::cout << "rgbaf(" << c.r << ", " << c.g << ", " << c.b << ", "
                  << c.a << ")";
    } else if (v.isRef()) {
        auto& ref = v.asRef();
        const char* gname = snoGroupName(static_cast<SnoGroup>(ref.group));
        std::cout << "sno_ref(";
        if (gname)
            std::cout << gname;
        else
            std::cout << "group=" << ref.group;
        std::cout << ", id=" << ref.snoId << ")";
    } else if (v.isGbid()) {
        auto& gbid = v.asGbid();
        std::cout << "gbid(group=" << gbid.group << ", hash=0x" << std::hex
                  << gbid.raw << std::dec << ")";
    } else if (v.isArray()) {
        auto& ta = v.asArray();
        size_t sz = ta.size();
        if (sz == 0) {
            std::cout << "[]";
            return;
        }
        std::cout << "[\n";
        size_t limit = std::min<size_t>(sz, 16);
        for (size_t i = 0; i < limit; ++i) {
            indent(depth + 1);
            std::cout << "[" << i << "] ";
            printTypedArrayElem(ta, i);
            std::cout << "\n";
        }
        if (sz > limit) {
            indent(depth + 1);
            std::cout << "... (" << sz << " total elements)\n";
        }
        indent(depth);
        std::cout << "]";
    } else if (v.isObject()) {
        size_t sz = v.size();
        if (sz == 0) {
            std::cout << "{}";
            return;
        }
        std::cout << "{\n";
        {
            auto& obj = v.asObject();
            int count = 0;
            for (auto& [key, val] : obj) {
                indent(depth + 1);
                std::cout << key << ": ";
                printValue(val, depth + 1, maxDepth);
                std::cout << "\n";
                if (++count >= 32 && obj.size() > 32) {
                    indent(depth + 1);
                    std::cout << "... (" << obj.size() << " fields total)\n";
                    break;
                }
            }
        }
        indent(depth);
        std::cout << "}";
    }
}

// ============================================================================
// JSON serialization
// ============================================================================

static void escapeJsonString(std::ostream& os, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    os << buf;
                } else {
                    os << c;
                }
                break;
        }
    }
}

static void jsonIndent(std::ostream& os, int depth) {
    for (int i = 0; i < depth; ++i)
        os << "  ";
}

static void writeJson(std::ostream& os, const whiteout::sno::SnoValue& v,
                      int depth);

/// Write a single typed array element as JSON to `os`.
static void writeJsonTypedElem(std::ostream& os, const whiteout::sno::SnoArray& ta,
                               size_t i, int depth) {
    using namespace whiteout::sno;
    switch (ta.kind()) {
    case SVT_BYTE:   os << static_cast<int>(ta.asByteData()[i]); break;
    case SVT_WORD:   os << ta.asWordData()[i]; break;
    case SVT_INT:    os << ta.asIntData()[i]; break;
    case SVT_UINT:   os << ta.asUintData()[i]; break;
    case SVT_FLOAT:  os << ta.asFloatData()[i]; break;
    case SVT_VEC2:   { auto& e = ta.asVec2Data()[i]; os << "[" << e.x << "," << e.y << "]"; break; }
    case SVT_VEC3:   { auto& e = ta.asVec3Data()[i]; os << "[" << e.x << "," << e.y << "," << e.z << "]"; break; }
    case SVT_VEC4:   { auto& e = ta.asVec4Data()[i]; os << "[" << e.x << "," << e.y << "," << e.z << "," << e.w << "]"; break; }
    case SVT_IVEC2:  { auto& e = ta.asIVec2Data()[i]; os << "[" << e.x << "," << e.y << "]"; break; }
    case SVT_COLOR:  { auto& c = ta.asColorData()[i]; os << "[" << (int)c.r << "," << (int)c.g << "," << (int)c.b << "," << (int)c.a << "]"; break; }
    case SVT_COLORF: { auto& c = ta.asColorFData()[i]; os << "[" << c.r << "," << c.g << "," << c.b << "," << c.a << "]"; break; }
    case SVT_REF:    { auto& e = ta.asRefData()[i]; os << "{\"group\":" << e.group << ",\"snoId\":" << e.snoId << "}"; break; }
    case SVT_GBID:   { auto& e = ta.asGbidData()[i]; os << "{\"group\":" << e.group << ",\"hash\":" << e.raw << "}"; break; }
    case SVT_ARRAY:
    case SVT_OBJECT: { writeJson(os, ta.asValueData()[i], depth); break; }
    default: os << "null"; break;
    }
}

static void writeJson(std::ostream& os, const whiteout::sno::SnoValue& v,
                      int depth = 0) {
    using namespace whiteout::sno;

    if (v.isNull()) {
        os << "null";
    } else if (v.isBool()) {
        os << (v.asBool() ? "true" : "false");
    } else if (v.isInt()) {
        os << v.asInt();
    } else if (v.isUint()) {
        os << v.asUint();
    } else if (v.isFloat()) {
        auto val = v.asFloat();
        // Handle special float values that aren't valid JSON.
        if (std::isnan(val)) os << "null";
        else if (std::isinf(val)) os << (val < 0 ? "-1e+38" : "1e+38");
        else os << val;
    } else if (v.isInt64()) {
        os << v.asInt64();
    } else if (v.isUint64()) {
        os << v.asUint64();
    } else if (v.isByte()) {
        os << static_cast<int>(v.asByte());
    } else if (v.isWord()) {
        os << v.asWord();
    } else if (v.isString()) {
        os << '"';
        escapeJsonString(os, v.asString());
        os << '"';
    } else if (v.isVec2()) {
        auto& vec = v.asVec2();
        os << "[" << vec.x << ", " << vec.y << "]";
    } else if (v.isVec3()) {
        auto& vec = v.asVec3();
        os << "[" << vec.x << ", " << vec.y << ", " << vec.z << "]";
    } else if (v.isVec4()) {
        auto& vec = v.asVec4();
        os << "[" << vec.x << ", " << vec.y << ", " << vec.z << ", " << vec.w
           << "]";
    } else if (v.isIVec2()) {
        auto& vec = v.asIVec2();
        os << "[" << vec.x << ", " << vec.y << "]";
    } else if (v.isColor()) {
        auto& c = v.asColor();
        os << "{\n";
        jsonIndent(os, depth + 1); os << "\"r\": " << (int)c.r << ",\n";
        jsonIndent(os, depth + 1); os << "\"g\": " << (int)c.g << ",\n";
        jsonIndent(os, depth + 1); os << "\"b\": " << (int)c.b << ",\n";
        jsonIndent(os, depth + 1); os << "\"a\": " << (int)c.a << "\n";
        jsonIndent(os, depth); os << "}";
    } else if (v.isColorF()) {
        auto& c = v.asColorF();
        os << "{\n";
        jsonIndent(os, depth + 1); os << "\"r\": " << c.r << ",\n";
        jsonIndent(os, depth + 1); os << "\"g\": " << c.g << ",\n";
        jsonIndent(os, depth + 1); os << "\"b\": " << c.b << ",\n";
        jsonIndent(os, depth + 1); os << "\"a\": " << c.a << "\n";
        jsonIndent(os, depth); os << "}";
    } else if (v.isRef()) {
        auto& ref = v.asRef();
        const char* gname = snoGroupName(static_cast<SnoGroup>(ref.group));
        os << "{\n";
        jsonIndent(os, depth + 1); os << "\"group\": ";
        if (gname) { os << '"'; escapeJsonString(os, gname); os << '"'; }
        else       os << ref.group;
        os << ",\n";
        jsonIndent(os, depth + 1); os << "\"snoId\": " << ref.snoId << "\n";
        jsonIndent(os, depth); os << "}";
    } else if (v.isGbid()) {
        auto& gbid = v.asGbid();
        os << "{\n";
        jsonIndent(os, depth + 1); os << "\"group\": " << gbid.group << ",\n";
        jsonIndent(os, depth + 1); os << "\"hash\": " << gbid.raw << "\n";
        jsonIndent(os, depth); os << "}";
    } else if (v.isArray()) {
        auto& ta = v.asArray();
        size_t sz = ta.size();
        if (sz == 0) {
            os << "[]";
            return;
        }
        os << "[\n";
        for (size_t i = 0; i < sz; ++i) {
            jsonIndent(os, depth + 1);
            writeJsonTypedElem(os, ta, i, depth + 1);
            if (i + 1 < sz) os << ',';
            os << '\n';
        }
        jsonIndent(os, depth); os << "]";
    } else if (v.isObject()) {
        size_t sz = v.size();
        if (sz == 0) {
            os << "{}";
            return;
        }
        os << "{\n";
        {
            auto& obj = v.asObject();
            size_t idx = 0;
            for (auto& [key, val] : obj) {
                jsonIndent(os, depth + 1);
                os << '"';
                escapeJsonString(os, key);
                os << "\": ";
                writeJson(os, val, depth + 1);
                if (++idx < obj.size()) os << ',';
                os << '\n';
            }
        }
        jsonIndent(os, depth); os << "}";
    }
}

static std::string toJson(const whiteout::sno::SnoFile& file) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"magic\": " << file.dwSignature << ",\n";
    os << "  \"formatHash\": " << file.formatHash << ",\n";
    os << "  \"snoId\": " << file.snoId << ",\n";
    os << "  \"typeName\": \"";
    escapeJsonString(os, file.typeName);
    os << "\",\n";
    os << "  \"root\": ";
    writeJson(os, file.root, 1);
    os << "\n}\n";
    return os.str();
}

static void promptSaveJson(const whiteout::sno::SnoFile& file,
                           const std::string& assetName) {
    std::cout << "\n  Save as JSON? [y/N]: ";
    std::string ans;
    std::getline(std::cin, ans);
    if (ans.empty() || (ans[0] != 'y' && ans[0] != 'Y'))
        return;

    // Default filename: <assetName>.json
    std::string defaultName = assetName + ".json";
    std::cout << "  Filename [" << defaultName << "]: ";
    std::string fname;
    std::getline(std::cin, fname);
    fname = fname.empty() ? defaultName
                          : (fname.find('.') == std::string::npos
                                 ? fname + ".json"
                                 : fname);

    std::string json = toJson(file);
    std::ofstream out(fname, std::ios::binary);
    if (!out) {
        std::cerr << "  Error: could not open \"" << fname
                  << "\" for writing.\n";
        return;
    }
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    out.close();
    std::cout << "  Saved " << json.size() << " bytes to " << fname << "\n";
}

// ============================================================================
// Helpers
// ============================================================================

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Resolve an SnoGroup from a file extension string (e.g. "app" → Appearance).
/// Returns SnoGroup::None if no match.
static whiteout::sno::SnoGroup snoGroupFromExtension(const std::string& ext) {
    using namespace whiteout::sno;
    // Iterate all known group IDs and compare extensions.
    for (int gid = -1; gid <= 180; ++gid) {
        auto g = static_cast<SnoGroup>(gid);
        const char* e = snoGroupExtension(g);
        if (e && ext == e)
            return g;
    }
    return SnoGroup::None;
}

/// Try to read an SNO from CASC by its TocEntry, parse it, and print the tree.
static void openSno(const whiteout::storages::casc::Storage& storage,
                    const whiteout::sno::SnoReader& reader,
                    const whiteout::sno::TocEntry& entry,
                    const CombinedMetaCache& combinedCache = {},
                    bool isD3 = false) {
    using namespace whiteout::sno;

    const char* gname = snoGroupName(entry.group);
    std::cout << "\n=== " << (gname ? gname : "Unknown") << " / "
              << entry.name << " (snoId=" << entry.snoId << ") ===\n";

    // Resolve the CASC path for the SNO file.
    //   D4:  base:meta\<snoId>  or  Base\meta\<GroupDir>\<Name>.<ext>
    //   D3:  <GroupDir>\<Name>.<ext>  (no prefix)
    std::optional<std::vector<whiteout::u8>> fileData;
    std::string snoPath;

    const char* groupDir = isD3 ? snoGroupDirD3(entry.group)
                                 : snoGroupDir(entry.group);
    const char* groupExt = snoGroupExtension(entry.group);

    auto trySnoPath = [&](const std::string& path) -> bool {
        if (fileData) return true;
        auto data = storage.readFile(path);
        if (data) { fileData = std::move(data); snoPath = path; return true; }
        return false;
    };

    if (isD3) {
        // D3: try the exact named path first — this is the most reliable
        // approach because the TOC snoId is NOT a CASC file-data-ID.
        if (groupDir && groupExt) {
            std::string relPath =
                std::string(groupDir) + "\\" + entry.name + "." + groupExt;
            trySnoPath("Base\\" + relPath);
            trySnoPath(relPath);
        }
        // Wildcard discovery fallback.
        if (!fileData) {
            std::vector<std::string> hits;
            storage.enumerate(
                [&](const whiteout::storages::casc::EnumerateEntry& fe) -> bool {
                    if (fe.path.find(entry.name) != std::string_view::npos)
                        hits.push_back(std::string(fe.path));
                    return hits.size() < 50;
                });
            if (!hits.empty()) {
                std::cout << "  CASC entries matching this asset:\n";
                for (auto& h : hits)
                    std::cout << "    " << h << "\n";

                // Build the expected suffix: <GroupDir>\<Name>.<ext>
                // Prefer the hit whose path ends with the correct group
                // directory and extension before falling back to any match.
                if (groupDir && groupExt) {
                    std::string suffix =
                        std::string(groupDir) + "\\" + entry.name + "." + groupExt;
                    for (auto& h : hits) {
                        if (h.size() >= suffix.size() &&
                            h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0) {
                            trySnoPath(h);
                            break;
                        }
                    }
                }
                // If exact group match didn't work, try any hit.
                if (!fileData) {
                    for (auto& h : hits)
                        trySnoPath(h);
                }
            }
        }
    } else {
        // D4: try named path, then numeric snoId.
        if (groupDir && groupExt) {
            trySnoPath(std::string("Base\\meta\\") + groupDir + "\\" +
                       entry.name + "." + groupExt);
        }
        trySnoPath("base:meta\\" + std::to_string(entry.snoId));
    }

    // Fallback: try combined meta cache (for Texture, StringList, etc.).
    if (!fileData && !isD3) {
        auto it = combinedCache.entries.find(entry.snoId);
        if (it != combinedCache.entries.end()) {
            // Resolve the format hash for this group.
            whiteout::u32 fmtHash = 0;
            auto fhIt = reader.groupFormatHashes().find(
                static_cast<whiteout::i32>(entry.group));
            if (fhIt != reader.groupFormatHashes().end())
                fmtHash = fhIt->second;

            auto synth = buildSnoFromCombinedEntry(it->second, fmtHash);
            fileData = std::move(synth);
            snoPath = "(combined meta)";
        }
    }

    if (!fileData) {
        std::cerr << "  Could not read SNO from CASC (error "
                  << whiteout::storages::casc::Storage::lastError() << ").\n";
        return;
    }

    std::cout << "  Size: " << fileData->size() << " bytes"
              << " (from " << snoPath << ")\n";

    // Try to load the associated payload file.  D4 stores external array
    // data in a separate payload file; D3 does not use payloads.
    std::optional<std::vector<whiteout::u8>> payloadFileData;
    std::string payloadPath;
    if (!isD3) {
        payloadPath = "base:payload\\" + std::to_string(entry.snoId);
        payloadFileData = storage.readFile(payloadPath);
        if (!payloadFileData) {
            payloadPath.clear();
        }
    }

    // Payload data span (skip 16-byte header if present, like the meta file).
    std::span<const whiteout::u8> payloadSpan;
    if (payloadFileData && payloadFileData->size() > 16) {
        whiteout::u32 payMagic = 0;
        std::memcpy(&payMagic, payloadFileData->data(), 4);
        if (payMagic == kSnoMagic) {
            // Payload file has a SNO header — skip it.
            payloadSpan = std::span<const whiteout::u8>(
                payloadFileData->data() + 16,
                payloadFileData->size() - 16);
        } else {
            // Raw payload, use as-is.
            payloadSpan = std::span<const whiteout::u8>(*payloadFileData);
        }
        std::cout << "  Payload: " << payloadFileData->size() << " bytes"
                  << " (from " << payloadPath << ")\n";
    }

    // Show raw header.
    if (fileData->size() >= sizeof(SnoHeader)) {
        SnoHeader hdr{};
        std::memcpy(&hdr, fileData->data(), sizeof(hdr));
        std::cout << "  Magic:       0x" << std::hex << hdr.magic << std::dec
                  << (hdr.magic == kSnoMagic ? " (OK)" : " (UNEXPECTED)") << "\n";
        std::cout << "  Format hash: 0x" << std::hex << hdr.formatHash
                  << std::dec << "\n";
        std::cout << "  SNO ID:      " << hdr.snoId << "\n";
    }

    // Parse the SNO into its value tree.  D3 files are detected automatically.
    std::optional<whiteout::sno::SnoFile> result;
    if (!payloadSpan.empty()) {
        result = reader.parse(*fileData, entry.group, payloadSpan);
    } else {
        result = reader.parse(*fileData, entry.group);
    }
    if (!result) {
        std::cout << "  (Parser could not deserialise this SNO — showing raw "
                     "header only.)\n";
        return;
    }

    std::cout << "  Type name:   " << result->typeName << "\n\n";
    printValue(result->root, 0, 4);
    std::cout << "\n";

    // Offer to save as JSON.
    promptSaveJson(*result, entry.name);
}

// ============================================================================
// Menu actions
// ============================================================================

/// Open an SNO by searching for its name in the CoreTOC.
static void actionOpenByName(const whiteout::storages::casc::Storage& storage,
                             const whiteout::sno::SnoReader& reader,
                             const whiteout::sno::CoreToc& toc,
                             const CombinedMetaCache& combinedCache,
                             bool isD3 = false) {
    using namespace whiteout::sno;

    std::cout << "Enter asset name (or partial name to search): ";
    std::string input;
    std::getline(std::cin, input);
    input = trim(input);
    if (input.empty()) {
        std::cout << "  (empty input, returning to menu)\n";
        return;
    }

    // If the input looks like a path (contains \ or /), extract the filename.
    {
        auto pos = input.find_last_of("\\/");
        if (pos != std::string::npos)
            input = input.substr(pos + 1);
    }

    // If the filename has an extension, try to resolve it to an SnoGroup for
    // filtering, then strip the extension from the search term.
    SnoGroup filterGroup = SnoGroup::None;
    {
        auto dot = input.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = input.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            filterGroup = snoGroupFromExtension(ext);
            input = input.substr(0, dot);
        }
    }

    if (filterGroup != SnoGroup::None) {
        const char* gn = snoGroupName(filterGroup);
        std::cout << "  (filtering to group: " << (gn ? gn : "?") << ")\n";
    }

    // Convert input to lowercase for case-insensitive matching.
    std::string inputLower = input;
    std::transform(inputLower.begin(), inputLower.end(), inputLower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Search through all entries for matches.
    std::vector<const TocEntry*> matches;
    for (auto& entry : toc.entries()) {
        // If the user provided an extension that resolved to a group, skip
        // entries from other groups.
        if (filterGroup != SnoGroup::None && entry.group != filterGroup)
            continue;

        std::string nameLower = entry.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (nameLower.find(inputLower) != std::string::npos) {
            matches.push_back(&entry);
        }
    }

    if (matches.empty()) {
        std::cout << "  No assets found matching \"" << input << "\".\n";
        return;
    }

    if (matches.size() == 1) {
        openSno(storage, reader, *matches[0], combinedCache, isD3);
        return;
    }

    // Multiple matches — let the user pick.
    const size_t maxShow = 25;
    std::cout << "\n  Found " << matches.size() << " matches";
    if (matches.size() > maxShow)
        std::cout << " (showing first " << maxShow << ")";
    std::cout << ":\n\n";

    size_t limit = std::min(matches.size(), maxShow);
    for (size_t i = 0; i < limit; ++i) {
        const char* gname = snoGroupName(matches[i]->group);
        std::cout << "  " << (i + 1) << ". [" << (gname ? gname : "?") << "] "
                  << matches[i]->name << " (id=" << matches[i]->snoId << ")\n";
    }

    std::cout << "\n  Enter number to open (or 0 to cancel): ";
    std::string choice;
    std::getline(std::cin, choice);
    choice = trim(choice);

    int idx = 0;
    try {
        idx = std::stoi(choice);
    } catch (...) {
        std::cout << "  Invalid selection.\n";
        return;
    }

    if (idx < 1 || idx > static_cast<int>(limit)) {
        std::cout << "  Cancelled.\n";
        return;
    }

    openSno(storage, reader, *matches[static_cast<size_t>(idx - 1)],
             combinedCache, isD3);
}

/// Open an SNO by its numeric SNO ID.
static void actionOpenById(const whiteout::storages::casc::Storage& storage,
                           const whiteout::sno::SnoReader& reader,
                           const whiteout::sno::CoreToc& toc,
                           const CombinedMetaCache& combinedCache,
                           bool isD3 = false) {
    std::cout << "Enter SNO ID: ";
    std::string input;
    std::getline(std::cin, input);
    input = trim(input);
    if (input.empty()) {
        std::cout << "  (empty input, returning to menu)\n";
        return;
    }

    whiteout::i32 snoId = 0;
    try {
        // Support hex input (0x...) and decimal.
        if (input.size() > 2 && input[0] == '0' &&
            (input[1] == 'x' || input[1] == 'X')) {
            snoId = static_cast<whiteout::i32>(
                std::stoul(input, nullptr, 16));
        } else {
            snoId = std::stoi(input);
        }
    } catch (...) {
        std::cerr << "  Invalid number: \"" << input << "\".\n";
        return;
    }

    auto* entry = toc.findById(snoId);
    if (!entry) {
        std::cerr << "  SNO ID " << snoId
                  << " not found in the CoreTOC.\n";
        return;
    }

    openSno(storage, reader, *entry, combinedCache, isD3);
}

/// Extract N random SNO files from a chosen group and save them to disk.
static void actionExtractRandomGroup(
    const whiteout::storages::casc::Storage& storage,
    const whiteout::sno::SnoReader& reader,
    const whiteout::sno::CoreToc& toc,
    const CombinedMetaCache& combinedCache,
    bool isD3) {
    using namespace whiteout::sno;

    // --- List available groups with entries ---
    struct GroupInfo {
        SnoGroup group;
        const char* name;
        size_t count;
    };
    std::vector<GroupInfo> available;
    for (int gid = -1; gid <= 180; ++gid) {
        auto g = static_cast<SnoGroup>(gid);
        auto entries = toc.entriesForGroup(g);
        if (entries.empty()) continue;
        const char* gname = snoGroupName(g);
        if (!gname) continue;
        available.push_back({g, gname, entries.size()});
    }

    if (available.empty()) {
        std::cout << "  No groups with entries found.\n";
        return;
    }

    std::cout << "\n  Available SNO groups:\n\n";
    for (size_t i = 0; i < available.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << available[i].name
                  << " (" << available[i].count << " entries)\n";
    }

    std::cout << "\n  Select group number (or 0 to cancel): ";
    std::string input;
    std::getline(std::cin, input);
    input = trim(input);
    if (input.empty()) { std::cout << "  Cancelled.\n"; return; }

    int groupIdx = 0;
    try { groupIdx = std::stoi(input); } catch (...) {
        std::cout << "  Invalid selection.\n";
        return;
    }
    if (groupIdx < 1 || groupIdx > static_cast<int>(available.size())) {
        std::cout << "  Cancelled.\n";
        return;
    }

    auto& chosen = available[static_cast<size_t>(groupIdx - 1)];
    auto groupEntries = toc.entriesForGroup(chosen.group);

    std::cout << "  How many files to extract? [all " << groupEntries.size() << "]: ";
    std::getline(std::cin, input);
    input = trim(input);

    size_t requestedCount = groupEntries.size();
    if (!input.empty()) {
        try { requestedCount = static_cast<size_t>(std::stoul(input)); } catch (...) {
            std::cout << "  Invalid number.\n";
            return;
        }
    }
    if (requestedCount == 0) {
        std::cout << "  Nothing to extract.\n";
        return;
    }

    std::cout << "  Also export JSON? [y/N]: ";
    std::getline(std::cin, input);
    input = trim(input);
    bool exportJson = (!input.empty() && (input[0] == 'y' || input[0] == 'Y'));

    // Clamp to available count.
    size_t totalAvailable = groupEntries.size();
    size_t extractCount = std::min(requestedCount, totalAvailable);
    if (extractCount < requestedCount) {
        std::cout << "  Only " << totalAvailable
                  << " entries available, extracting all.\n";
    }

    // Build indices and shuffle for random selection.
    std::vector<size_t> indices(totalAvailable);
    std::iota(indices.begin(), indices.end(), 0);
    if (extractCount < totalAvailable) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::shuffle(indices.begin(), indices.end(), rng);
    }
    indices.resize(extractCount);

    // Create output directories.
    std::string metaDir = std::string("meta/") + chosen.name;
    std::string payloadDir = std::string("payload/") + chosen.name;
    std::string paylowDir = std::string("paylow/") + chosen.name;
    std::filesystem::create_directories(metaDir);
    bool payloadDirCreated = false;
    bool paylowDirCreated = false;
    const bool isTexture = (chosen.group == SnoGroup::Texture);

    const char* groupExt = snoGroupExtension(chosen.group);
    const char* groupDir = isD3 ? snoGroupDirD3(chosen.group)
                                : snoGroupDir(chosen.group);

    std::cout << "  Extracting " << extractCount << " "
              << chosen.name << " files to " << metaDir << "/\n";

    size_t saved = 0;
    size_t failed = 0;
    std::vector<std::string> failedEntries;
    for (size_t idx : indices) {
        auto& entry = groupEntries[idx];

        // Try to read from CASC.
        std::optional<std::vector<whiteout::u8>> fileData;

        auto tryRead = [&](const std::string& path) -> bool {
            if (fileData) return true;
            auto data = storage.readFile(path);
            if (data) { fileData = std::move(data); return true; }
            return false;
        };

        if (isD3) {
            if (groupDir && groupExt) {
                std::string relPath = std::string(groupDir) + "\\" +
                                      entry.name + "." + groupExt;
                tryRead("Base\\" + relPath);
                tryRead(relPath);
            }
        } else {
            if (groupDir && groupExt) {
                tryRead(std::string("Base\\meta\\") + groupDir + "\\" +
                        entry.name + "." + groupExt);
            }
            tryRead("base:meta\\" + std::to_string(entry.snoId));
        }

        // Fallback: combined meta cache.
        if (!fileData && !isD3) {
            auto it = combinedCache.entries.find(entry.snoId);
            if (it != combinedCache.entries.end()) {
                whiteout::u32 fmtHash = 0;
                auto fhIt = reader.groupFormatHashes().find(
                    static_cast<whiteout::i32>(entry.group));
                if (fhIt != reader.groupFormatHashes().end())
                    fmtHash = fhIt->second;
                fileData = buildSnoFromCombinedEntry(it->second, fmtHash);
            }
        }

        if (!fileData || fileData->empty()) {
            failedEntries.push_back(
                entry.name + " (snoId=" + std::to_string(entry.snoId) +
                ") — not found in CASC (error " +
                std::to_string(whiteout::storages::casc::Storage::lastError()) + ")");
            ++failed;
            continue;
        }

        // Build output filename.
        std::string ext = groupExt ? groupExt : "sno";
        std::string metaPath = metaDir + "/" + entry.name + "." + ext;

        // Save meta file.
        {
            std::ofstream out(metaPath, std::ios::binary);
            if (!out) {
                failedEntries.push_back(
                    entry.name + " (snoId=" + std::to_string(entry.snoId) +
                    ") — could not write to " + metaPath);
                ++failed;
                continue;
            }
            out.write(reinterpret_cast<const char*>(fileData->data()),
                      static_cast<std::streamsize>(fileData->size()));
        }
        ++saved;

        // Try to load and save payload for D4.
        std::span<const whiteout::u8> payloadSpan;
        std::optional<std::vector<whiteout::u8>> payloadFileData;
        if (!isD3) {
            payloadFileData = storage.readFile(
                "base:payload\\" + std::to_string(entry.snoId));
            if (payloadFileData && !payloadFileData->empty()) {
                // Create payload directory on first use.
                if (!payloadDirCreated) {
                    std::filesystem::create_directories(payloadDir);
                    payloadDirCreated = true;
                }

                // Save raw payload file.
                std::string payPath = payloadDir + "/" + entry.name + "." + ext;
                std::ofstream pout(payPath, std::ios::binary);
                if (pout) {
                    pout.write(
                        reinterpret_cast<const char*>(payloadFileData->data()),
                        static_cast<std::streamsize>(payloadFileData->size()));
                }

                // Prepare payload span for parsing (skip SNO header if present).
                if (payloadFileData->size() > 16) {
                    whiteout::u32 payMagic = 0;
                    std::memcpy(&payMagic, payloadFileData->data(), 4);
                    if (payMagic == whiteout::sno::kSnoMagic)
                        payloadSpan = std::span<const whiteout::u8>(
                            payloadFileData->data() + 16,
                            payloadFileData->size() - 16);
                    else
                        payloadSpan = std::span<const whiteout::u8>(
                            *payloadFileData);
                }
            }

            // For Textures, also dump the 2nd payload (low-res tier)
            // from base:paylow\<snoId>.
            if (isTexture) {
                auto paylowFileData = storage.readFile(
                    "base:paylow\\" + std::to_string(entry.snoId));
                if (paylowFileData && !paylowFileData->empty()) {
                    if (!paylowDirCreated) {
                        std::filesystem::create_directories(paylowDir);
                        paylowDirCreated = true;
                    }
                    std::string paylowPath =
                        paylowDir + "/" + entry.name + "." + ext;
                    std::ofstream plout(paylowPath, std::ios::binary);
                    if (plout) {
                        plout.write(
                            reinterpret_cast<const char*>(
                                paylowFileData->data()),
                            static_cast<std::streamsize>(
                                paylowFileData->size()));
                    }
                }
            }
        }

        // Optionally parse and write JSON.
        if (exportJson) {
            auto parsed = !payloadSpan.empty()
                ? reader.parse(*fileData, entry.group, payloadSpan)
                : reader.parse(*fileData, entry.group);

            if (parsed) {
                std::string jsonPath = metaPath + ".json";
                std::ofstream jout(jsonPath, std::ios::binary);
                if (jout) {
                    std::string json = toJson(*parsed);
                    jout.write(json.data(),
                               static_cast<std::streamsize>(json.size()));
                }
            }
        }

        // Progress.
        if ((saved + failed) % 50 == 0 || (saved + failed) == extractCount) {
            std::cout << "  Progress: " << (saved + failed) << " / "
                      << extractCount << " (" << saved << " saved, "
                      << failed << " failed)\r" << std::flush;
        }
    }

    std::cout << "\n  Done. Saved " << saved << " meta files to " << metaDir
              << "/ (" << failed << " failed).\n";

    // Write a failure log if there were any failures.
    if (!failedEntries.empty()) {
        std::string logPath = metaDir + "/_failed.log";
        std::ofstream log(logPath);
        if (log) {
            for (auto& line : failedEntries)
                log << line << "\n";
            log.close();
            std::cout << "  Failure details written to " << logPath << "\n";
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    using namespace whiteout;
    using namespace whiteout::sno;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <casc_storage_path>\n"
                  << "\nOpens a CASC storage and lets you interactively browse\n"
                  << "SNO assets by name or by numeric SNO ID.\n"
                  << "\nExamples:\n"
                  << "  " << argv[0]
                  << R"( "C:\Program Files (x86)\Diablo IV")" << "\n"
                  << "  " << argv[0]
                  << R"( "C:\Games\D4\Data")" << "\n";
        return 1;
    }

    const std::string storagePath = argv[1];

    // -----------------------------------------------------------------
    // 1.  Open the CASC storage.
    // -----------------------------------------------------------------
    std::cout << "Opening CASC storage: " << storagePath << "\n";
    auto storageOpt = storages::casc::Storage::open(storagePath);
    if (!storageOpt) {
        std::cerr << "Failed to open CASC storage (error "
                  << storages::casc::Storage::lastError() << ").\n";
        return 1;
    }
    auto& storage = *storageOpt;
    std::cout << "  CASC storage opened successfully.\n";

    // -----------------------------------------------------------------
    // 2.  Read and parse the CoreTOC (Table of Contents).
    // -----------------------------------------------------------------
    std::cout << "Reading CoreTOC.dat ...\n";

    // Try several known CASC path conventions.
    std::optional<std::vector<whiteout::u8>> tocData;
    std::string tocPath;
    auto tryToc = [&](const std::string& path) -> bool {
        if (tocData) return true;
        auto data = storage.readFile(path);
        if (data) { tocData = std::move(data); tocPath = path; return true; }
        return false;
    };
    tryToc("base:CoreTOC.dat");                    // D4
    tryToc("CoreTOC.dat");                          // bare
    tryToc("data\\pc\\misc\\CoreTOC.dat");            // D3 common
    tryToc("Data_D3\\PC\\Misc\\CoreTOC.dat");        // D3 alt

    // Last resort: wildcard search.
    if (!tocData) {
        std::vector<std::string> tocHits;
        storage.enumerate(
            [&](const storages::casc::EnumerateEntry& fe) -> bool {
                if (fe.path.find("CoreTOC") != std::string_view::npos)
                    tocHits.push_back(std::string(fe.path));
                return tocHits.size() < 5;
            });
        for (auto& h : tocHits) {
            if (tryToc(h)) {
                std::cout << "  (discovered at " << h << ")\n";
                break;
            }
        }
    }

    if (!tocData) {
        std::cerr << "Failed to read CoreTOC.dat (error "
                  << storages::casc::Storage::lastError() << ").\n";
        return 1;
    }
    std::cout << "  Loaded from: " << tocPath << "\n";

    CoreToc toc;
    if (!toc.parse(*tocData)) {
        std::cerr << "Failed to parse CoreTOC.dat.\n";
        return 1;
    }

    const char* fmtName = "Unknown";
    switch (toc.format()) {
        case CoreTocFormat::D3Legacy: fmtName = "D3 Legacy"; break;
        case CoreTocFormat::D4Old:    fmtName = "D4 Old";    break;
        case CoreTocFormat::D4New:    fmtName = "D4 New";    break;
        default: break;
    }

    const bool isD3 = (toc.format() == CoreTocFormat::D3Legacy);
    std::cout << "  Format:      " << fmtName << "\n";
    std::cout << "  Entries:     " << toc.size() << "\n";

    // Print group summary.
    std::cout << "\n  SNO Group Summary:\n";
    int groupCount = 0;
    for (i32 gid = -1; gid <= 180; ++gid) {
        auto entries = toc.entriesForGroup(static_cast<SnoGroup>(gid));
        if (entries.empty()) continue;
        const char* gname = snoGroupName(static_cast<SnoGroup>(gid));
        std::string label = gname ? gname : ("Group_" + std::to_string(gid));
        std::cout << "    " << label << ": " << entries.size() << " entries\n";
        ++groupCount;
    }
    std::cout << "  (" << groupCount << " active groups)\n";

    // -----------------------------------------------------------------
    // 3.  Set up the SNO reader.
    // -----------------------------------------------------------------
    SnoReader reader;
    reader.setFormatHashes(toc.formatHashes());

    // -----------------------------------------------------------------
    // 3b. Load combined meta files (for Texture, StringList, etc.).
    // -----------------------------------------------------------------
    CombinedMetaCache combinedCache;
    if (!isD3) {
        std::cout << "\nLoading combined meta files ...\n";
        loadCombinedMetas(storage, combinedCache);
    }

    // -----------------------------------------------------------------
    // 4.  Interactive menu loop.
    // -----------------------------------------------------------------
    std::cout << "\n";
    while (true) {
        std::cout << "------------------------------\n"
                  << "  1. Open SNO by name\n"
                  << "  2. Open SNO by SNO ID\n"
                  << "  3. Extract random files from a group\n"
                  << "  4. Exit\n"
                  << "------------------------------\n"
                  << "Choice: ";

        std::string choice;
        if (!std::getline(std::cin, choice)) break; // EOF
        choice = trim(choice);

        if (choice == "1") {
            actionOpenByName(storage, reader, toc, combinedCache, isD3);
        } else if (choice == "2") {
            actionOpenById(storage, reader, toc, combinedCache, isD3);
        } else if (choice == "3") {
            actionExtractRandomGroup(storage, reader, toc, combinedCache, isD3);
        } else if (choice == "4" || choice == "exit" || choice == "quit") {
            std::cout << "Bye.\n";
            break;
        } else {
            std::cout << "  Invalid choice. Enter 1, 2, 3, or 4.\n";
        }

        std::cout << "\n";
    }

    return 0;
}
