// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file binary.cpp
 * @brief The binary FBX encoding, both ways.
 *
 * A record is `EndOffset, NumProperties, PropertyListLen` (u32 each before
 * 7.5, u64 from 7.5), a length-prefixed name, the properties, the children,
 * and a zeroed record closing any child list. The writer's conventions are the
 * ones measured in SDK-written files (FBX_OBJ_DESIGN §0): every record with
 * children closes them with that null record and no leaf does; arrays of a
 * kilobyte and more are deflated; and the footer is the creation-time code,
 * zero padding to 16, four zero bytes, the version, 120 zero bytes and the
 * fixed magic.
 */

#include "whiteout/models/fbx/fbx.h"

#include <cstring>

#include "../../common/deflate.h"

namespace whiteout {
namespace models {
namespace fbx {

namespace {

constexpr u8 kHeader[23] = {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F', 'B', 'X', ' ',
                            'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', 0x00, 0x1A, 0x00};
constexpr u8 kFooterMagic[16] = {0xF8, 0x5A, 0x8C, 0x6A, 0xDE, 0xF5, 0xD9, 0x7E,
                                 0xEC, 0xE9, 0x0C, 0xE3, 0x75, 0x8F, 0x29, 0x0B};
/// The SDK deflates arrays from about here (measured: 1016 bytes raw, 1040 deflated).
constexpr std::size_t kDeflateFrom = 1024;
constexpr u32 kMaxDepth = 64;
/// DEFLATE cannot expand past about 1032:1, so a declared size beyond that of
/// the compressed bytes is a lie, refused before anything is allocated.
constexpr u64 kMaxInflateRatio = 1032;

template <class T>
T Load(const u8* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

class BinaryReader {
public:
    BinaryReader(std::span<const u8> data, bool wide) : data_(data), wide_(wide) {}

    std::string error;
    std::vector<std::string> warnings;

    /// Reads the record at @p offset, which must end by @p limit. Sets @p null
    /// for a closing null record. False on a malformed record.
    bool readNode(u64& offset, u64 limit, Node& out, bool& null, u32 depth) {
        const u64 header = wide_ ? 25 : 13;
        if (offset + header > limit) {
            return fail("a record header runs past its parent");
        }
        const u8* p = data_.data() + offset;
        const u64 end = wide_ ? Load<u64>(p) : Load<u32>(p);
        const u64 count = wide_ ? Load<u64>(p + 8) : Load<u32>(p + 4);
        const u64 listLength = wide_ ? Load<u64>(p + 16) : Load<u32>(p + 8);
        const u8 nameLength = p[wide_ ? 24 : 12];
        if (end == 0) {
            null = true;
            offset += header;
            return true;
        }
        null = false;
        if (depth > kMaxDepth) {
            return fail("records nest deeper than any FBX writer goes");
        }
        if (end <= offset || end > limit) {
            return fail("a record ends outside its parent");
        }
        u64 cursor = offset + header;
        if (cursor + nameLength > end) {
            return fail("a record name runs past the record");
        }
        out.name.assign(reinterpret_cast<const char*>(data_.data() + cursor), nameLength);
        cursor += nameLength;
        const u64 listEnd = cursor + listLength;
        if (listEnd > end || count > listLength) {
            return fail("a property list runs past its record");
        }
        out.properties.reserve(static_cast<std::size_t>(count));
        for (u64 i = 0; i < count; ++i) {
            Property property;
            if (!readProperty(cursor, listEnd, property)) {
                return false;
            }
            out.properties.push_back(std::move(property));
        }
        if (cursor != listEnd) {
            return fail("a property list's length disagrees with its properties");
        }
        while (cursor < end) {
            Node child;
            bool childNull = false;
            if (!readNode(cursor, end, child, childNull, depth + 1)) {
                return false;
            }
            if (childNull) {
                out.block = true;
                break;
            }
            out.children.push_back(std::move(child));
        }
        offset = end;
        return true;
    }

private:
    bool fail(const char* message) {
        if (error.empty()) {
            error = message;
        }
        return false;
    }

    template <class T>
    bool readArray(u64& cursor, u64 limit, std::vector<T>& out) {
        if (cursor + 12 > limit) {
            return fail("an array header runs past its record");
        }
        const u8* p = data_.data() + cursor;
        const u64 count = Load<u32>(p);
        const u32 encoding = Load<u32>(p + 4);
        const u64 compressed = Load<u32>(p + 8);
        cursor += 12;
        if (cursor + compressed > limit) {
            return fail("an array's bytes run past its record");
        }
        const u64 size = count * sizeof(T);
        const std::span<const u8> bytes = data_.subspan(static_cast<std::size_t>(cursor),
                                                        static_cast<std::size_t>(compressed));
        cursor += compressed;
        if (encoding == 0) {
            if (compressed != size) {
                return fail("an array's byte count disagrees with its length");
            }
            out.resize(static_cast<std::size_t>(count));
            if (size != 0) {
                std::memcpy(out.data(), bytes.data(), static_cast<std::size_t>(size));
            }
            return true;
        }
        if (encoding != 1) {
            return fail("an array in an unknown encoding");
        }
        if (size > compressed * kMaxInflateRatio + 1024) {
            return fail("an array claims more data than its compressed bytes can hold");
        }
        std::string inflateError;
        const std::vector<u8> inflated =
            zlib_decompress(bytes, &inflateError, static_cast<std::size_t>(size));
        if (inflated.size() != size) {
            return fail("an array did not inflate to its declared length");
        }
        out.resize(static_cast<std::size_t>(count));
        if (size != 0) {
            std::memcpy(out.data(), inflated.data(), static_cast<std::size_t>(size));
        }
        return true;
    }

    bool readProperty(u64& cursor, u64 limit, Property& out) {
        if (cursor + 1 > limit) {
            return fail("a property runs past its record");
        }
        const char code = static_cast<char>(data_[static_cast<std::size_t>(cursor)]);
        ++cursor;
        const auto scalar = [&](u64 size) {
            if (cursor + size > limit) {
                return fail("a property value runs past its record");
            }
            return true;
        };
        const u8* p = data_.data() + cursor;
        switch (code) {
        case 'C':
            if (!scalar(1)) {
                return false;
            }
            out.type = PropertyType::Bool;
            out.integer = p[0];
            cursor += 1;
            return true;
        case 'Y':
            if (!scalar(2)) {
                return false;
            }
            out.type = PropertyType::I16;
            out.integer = Load<i16>(p);
            cursor += 2;
            return true;
        case 'I':
            if (!scalar(4)) {
                return false;
            }
            out.type = PropertyType::I32;
            out.integer = Load<i32>(p);
            cursor += 4;
            return true;
        case 'L':
            if (!scalar(8)) {
                return false;
            }
            out.type = PropertyType::I64;
            out.integer = Load<i64>(p);
            cursor += 8;
            return true;
        case 'F':
            if (!scalar(4)) {
                return false;
            }
            out.type = PropertyType::F32;
            out.number = Load<f32>(p);
            cursor += 4;
            return true;
        case 'D':
            if (!scalar(8)) {
                return false;
            }
            out.type = PropertyType::F64;
            out.number = Load<f64>(p);
            cursor += 8;
            return true;
        case 'S':
        case 'R': {
            if (!scalar(4)) {
                return false;
            }
            const u64 length = Load<u32>(p);
            cursor += 4;
            if (cursor + length > limit) {
                return fail("a string runs past its record");
            }
            out.type = code == 'S' ? PropertyType::String : PropertyType::Raw;
            out.text.assign(reinterpret_cast<const char*>(data_.data() + cursor),
                            static_cast<std::size_t>(length));
            cursor += length;
            return true;
        }
        case 'b':
            out.type = PropertyType::BoolArray;
            return readArray(cursor, limit, out.bools);
        case 'i':
            out.type = PropertyType::I32Array;
            return readArray(cursor, limit, out.i32s);
        case 'l':
            out.type = PropertyType::I64Array;
            return readArray(cursor, limit, out.i64s);
        case 'f':
            out.type = PropertyType::F32Array;
            return readArray(cursor, limit, out.f32s);
        case 'd':
            out.type = PropertyType::F64Array;
            return readArray(cursor, limit, out.f64s);
        default:
            return fail("a property of an unknown type");
        }
    }

    std::span<const u8> data_;
    bool wide_;
};

// ----------------------------------------------------------------------------

class BinaryWriter {
public:
    explicit BinaryWriter(bool wide) : wide_(wide) {}

    std::vector<u8> out;

    template <class T>
    void put(T value) {
        const std::size_t at = out.size();
        out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &value, sizeof(T));
    }

    void bytes(const void* data, std::size_t size) {
        const u8* p = static_cast<const u8*>(data);
        out.insert(out.end(), p, p + size);
    }

    void zeros(std::size_t count) {
        out.insert(out.end(), count, u8{0});
    }

    void nullRecord() {
        zeros(wide_ ? 25 : 13);
    }

    void node(const Node& node) {
        const std::size_t start = out.size();
        nullRecord(); // header, patched below
        const u8 nameLength = static_cast<u8>(node.name.size() < 255 ? node.name.size() : 255);
        out[start + (wide_ ? 24 : 12)] = nameLength;
        bytes(node.name.data(), nameLength);
        const std::size_t listStart = out.size();
        for (const Property& property : node.properties) {
            this->property(property);
        }
        const u64 listLength = out.size() - listStart;
        for (const Node& child : node.children) {
            this->node(child);
        }
        if (!node.children.empty() || node.block) {
            nullRecord();
        }
        const u64 end = out.size();
        const u64 count = node.properties.size();
        if (wide_) {
            std::memcpy(out.data() + start, &end, 8);
            std::memcpy(out.data() + start + 8, &count, 8);
            std::memcpy(out.data() + start + 16, &listLength, 8);
        } else {
            const u32 end32 = static_cast<u32>(end);
            const u32 count32 = static_cast<u32>(count);
            const u32 list32 = static_cast<u32>(listLength);
            std::memcpy(out.data() + start, &end32, 4);
            std::memcpy(out.data() + start + 4, &count32, 4);
            std::memcpy(out.data() + start + 8, &list32, 4);
        }
    }

private:
    template <class T>
    void array(char code, const std::vector<T>& values) {
        put<char>(code);
        put<u32>(static_cast<u32>(values.size()));
        const std::size_t size = values.size() * sizeof(T);
        if (size >= kDeflateFrom) {
            const std::vector<u8> deflated = zlib_compress(
                std::span<const u8>(reinterpret_cast<const u8*>(values.data()), size), nullptr);
            put<u32>(1);
            put<u32>(static_cast<u32>(deflated.size()));
            bytes(deflated.data(), deflated.size());
        } else {
            put<u32>(0);
            put<u32>(static_cast<u32>(size));
            bytes(values.data(), size);
        }
    }

    void property(const Property& p) {
        switch (p.type) {
        case PropertyType::Bool:
            put<char>('C');
            // The byte as stored: some fields hold a character (`Shading` is 'T').
            put<u8>(static_cast<u8>(p.integer));
            break;
        case PropertyType::I16:
            put<char>('Y');
            put<i16>(static_cast<i16>(p.integer));
            break;
        case PropertyType::I32:
            put<char>('I');
            put<i32>(static_cast<i32>(p.integer));
            break;
        case PropertyType::I64:
            put<char>('L');
            put<i64>(p.integer);
            break;
        case PropertyType::F32:
            put<char>('F');
            put<f32>(static_cast<f32>(p.number));
            break;
        case PropertyType::F64:
            put<char>('D');
            put<f64>(p.number);
            break;
        case PropertyType::String:
        case PropertyType::Raw:
            put<char>(p.type == PropertyType::String ? 'S' : 'R');
            put<u32>(static_cast<u32>(p.text.size()));
            bytes(p.text.data(), p.text.size());
            break;
        case PropertyType::BoolArray:
            array('b', p.bools);
            break;
        case PropertyType::I32Array:
            array('i', p.i32s);
            break;
        case PropertyType::I64Array:
            array('l', p.i64s);
            break;
        case PropertyType::F32Array:
            array('f', p.f32s);
            break;
        case PropertyType::F64Array:
            array('d', p.f64s);
            break;
        }
    }

    bool wide_;
};

} // namespace

ReadOutcome ReadBinary(std::span<const u8> data) {
    ReadOutcome outcome;
    if (!LooksLikeBinaryFbx(data)) {
        outcome.error = "not a binary FBX file";
        return outcome;
    }
    File file;
    file.binary = true;
    file.version = Load<u32>(data.data() + 23);
    if (file.version < 7000) {
        outcome.error = "FBX " + std::to_string(file.version) +
                        " predates 7.0; files from before the 2011 SDK are not read";
        return outcome;
    }
    BinaryReader reader(data, file.version >= 7500);
    u64 offset = 27;
    bool sawNull = false;
    while (offset < data.size()) {
        Node node;
        bool null = false;
        if (!reader.readNode(offset, data.size(), node, null, 0)) {
            outcome.error = reader.error + " (at byte " + std::to_string(offset) + ")";
            return outcome;
        }
        if (null) {
            sawNull = true;
            break;
        }
        file.nodes.push_back(std::move(node));
    }
    if (!sawNull) {
        outcome.warnings.push_back("the file ends without its closing null record");
    } else if (offset + 16 <= data.size()) {
        file.footerCode.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                               data.begin() + static_cast<std::ptrdiff_t>(offset + 16));
    }
    outcome.warnings.insert(outcome.warnings.end(), reader.warnings.begin(), reader.warnings.end());
    outcome.file = std::move(file);
    return outcome;
}

std::vector<u8> WriteBinary(const File& file) {
    BinaryWriter writer(file.version >= 7500);
    writer.bytes(kHeader, sizeof(kHeader));
    writer.put<u32>(file.version);
    for (const Node& node : file.nodes) {
        writer.node(node);
    }
    writer.nullRecord();
    // The footer: the creation-time code, then zeros up to a 16-byte boundary
    // (a whole 16 when already on one), four zero bytes, the version, 120
    // zero bytes and the magic.
    if (file.footerCode.size() == 16) {
        writer.bytes(file.footerCode.data(), 16);
    } else {
        writer.zeros(16);
    }
    std::size_t padding = (16 - writer.out.size() % 16) % 16;
    if (padding == 0) {
        padding = 16;
    }
    writer.zeros(padding);
    writer.zeros(4);
    writer.put<u32>(file.version);
    writer.zeros(120);
    writer.bytes(kFooterMagic, sizeof(kFooterMagic));
    return std::move(writer.out);
}

} // namespace fbx
} // namespace models
} // namespace whiteout
