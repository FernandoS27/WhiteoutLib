// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/sno/sno_reader.h>
#include "d3/sno_defs.h"
#include "d4/sno_defs.h"

#include "../common/binary_reader.h"
#include "../common/streams.h"

#include <algorithm>
#include <cstring>
#include <istream>

namespace whiteout {
namespace sno {

// ============================================================================
// Helpers
// ============================================================================

using common::BinaryReader;
using common::span_streambuf;

/// Read a trivially-copyable value at an absolute offset.
template <common::BinaryBlob T>
static T readAt(BinaryReader& r, size_t offset) {
    r.setPosition(static_cast<u32>(offset));
    return r.read<T>();
}

// Well-known basic type hashes (from definitions.json)
namespace TypeHash {
constexpr u32 DT_NULL = 1028442418;
constexpr u32 DT_BYTE = 1028015787;
constexpr u32 DT_WORD = 1028759507;
constexpr u32 DT_ENUM = 1028111660;
constexpr u32 DT_INT = 2764320258;
constexpr u32 DT_UINT = 1028680983;
constexpr u32 DT_FLOAT = 3864020909;
constexpr u32 DT_INT64 = 3867655596;
constexpr u32 DT_ACD_NETWORK_NAME = 2866333320;
constexpr u32 DT_SHARED_SERVER_DATA_ID = 3045283369;
constexpr u32 DT_SNO = 2764331143;
constexpr u32 DT_SNO_NAME = 3339108615;
constexpr u32 DT_GBID = 1028170061;
constexpr u32 DT_STARTLOC_NAME = 2193642883;
constexpr u32 DT_CSTRING = 3846829457;
constexpr u32 DT_CHARARRAY = 2175310548;
constexpr u32 DT_STRING_FORMULA = 2450313795;
constexpr u32 DT_RGBACOLOR = 2384880434;
constexpr u32 DT_RGBACOLORVALUE = 3212271855;
constexpr u32 DT_BCVEC2I = 1931092405;
constexpr u32 DT_VECTOR2D = 3124492544;
constexpr u32 DT_VECTOR3D = 3124492577;
constexpr u32 DT_VECTOR4D = 3124492610;
constexpr u32 DT_OPTIONAL = 3121633597;
constexpr u32 DT_RANGE = 3877855748;
constexpr u32 DT_FIXEDARRAY = 2388214534;
constexpr u32 DT_TAGMAP = 3493213809;
constexpr u32 DT_VARIABLEARRAY = 3244749660;
constexpr u32 DT_POLYMORPHIC_VARIABLEARRAY = 1683664497;
constexpr u32 DT_BINDABLEPROPERTY = 322094989;
} // namespace TypeHash

// ============================================================================
// Forward declaration of the recursive reader
// ============================================================================

enum class SnoFormat { D3, D4 };

struct ReadCtx {
    BinaryReader& reader;            // Reader over the payload (from offset 16)
    BinaryReader* payloadDataReader; // Reader over external payload data (nullptr if none)
    const SnoTypeRegistry& reg;
    SnoFormat format = SnoFormat::D4;
    size_t payloadSize = 0;     // Total payload buffer size
    size_t payloadDataSize = 0; // Total external payload buffer size
    size_t readLength = 0;      // Tracks bytes consumed

    /// Create a child context with readLength reset to 0.
    ReadCtx sub() const {
        return {reader, payloadDataReader, reg, format, payloadSize, payloadDataSize, 0};
    }
    /// Create a child context that reads from the external payload buffer.
    ReadCtx forExternalPayload() const {
        return {*payloadDataReader, payloadDataReader, reg, format,
                payloadDataSize,    payloadDataSize,   0};
    }
};

static SnoValue readStructure(ReadCtx& ctx, const u32 typeHashes[3], size_t offset,
                              const SnoFieldDef* field);

// ============================================================================
// Alignment helpers (matching parse.js getBasicTypeAlignment / getTypeAlignment)
// ============================================================================

static size_t getTypeAlignment(const SnoTypeRegistry& reg, const u32 typeHashes[3],
                               bool inTagMap = false);

/// Build the sub-type triple {typeHashes[1], typeHashes[2], DT_NULL}.
static void subTypeHashes(const u32 typeHashes[3], u32 out[3]) {
    out[0] = typeHashes[1];
    out[1] = typeHashes[2];
    out[2] = TypeHash::DT_NULL;
}

/// Check whether a field carries external payload data.
static bool isExternalField(const SnoFieldDef* field) {
    return field && (field->flags & (0x200000 | 0x400000));
}

/// Read a trivially-copyable value, advance readLength, return SnoValue.
template <typename T>
static SnoValue readSimple(ReadCtx& ctx, size_t offset) {
    ctx.readLength += sizeof(T);
    return SnoValue(readAt<T>(ctx.reader, offset));
}

/// Read an integer value with optional bool promotion, advance readLength.
template <typename T>
static SnoValue readInt(ReadCtx& ctx, size_t offset, const SnoFieldDef* field) {
    ctx.readLength += sizeof(T);
    return readIntValue<T>(ctx.reader, offset, field);
}

static size_t getBasicTypeAlignment(const SnoTypeDef* typeDef, const u32 typeHashes[3],
                                    const SnoTypeRegistry& reg, bool inTagMap) {
    switch (typeDef->hash) {
    case TypeHash::DT_POLYMORPHIC_VARIABLEARRAY:
    case TypeHash::DT_STRING_FORMULA:
    case TypeHash::DT_VARIABLEARRAY:
    case TypeHash::DT_TAGMAP:
    case TypeHash::DT_CSTRING:
        return inTagMap ? 4 : 8;
    case TypeHash::DT_CHARARRAY:
        return 1;
    case TypeHash::DT_FIXEDARRAY:
    case TypeHash::DT_OPTIONAL:
    case TypeHash::DT_RANGE: {
        u32 sub[3];
        subTypeHashes(typeHashes, sub);
        return getTypeAlignment(reg, sub, inTagMap);
    }
    case TypeHash::DT_SNO_NAME:
        return 4;
    case TypeHash::DT_BINDABLEPROPERTY:
        return 8;
    default:
        return typeDef->size > 0 ? typeDef->size : 4;
    }
}

static size_t getTypeAlignment(const SnoTypeRegistry& reg, const u32 typeHashes[3], bool inTagMap) {
    auto* typeDef = reg.findType(typeHashes[0]);
    if (!typeDef)
        return 4;

    if (typeDef->isBasic) {
        return getBasicTypeAlignment(typeDef, typeHashes, reg, inTagMap);
    }

    // Complex type: max alignment of all fields
    auto fields = reg.fields(*typeDef);
    if (fields.empty())
        return 4;

    size_t maxAlign = 1;
    for (auto& f : fields) {
        maxAlign = std::max(maxAlign, getTypeAlignment(reg, f.typeHashes, inTagMap));
    }
    return maxAlign;
}

// ============================================================================
// Typed-array helpers — bulk-read homogeneous basic-type arrays
// ============================================================================

/// Returns the fixed byte size for a basic type eligible for SnoArray,
/// or 0 if the type is not eligible (complex type, has sentinel null values,
/// or the field might promote elements to bool).
static size_t typedArrayElemSize(u32 elemTypeHash, const SnoFieldDef* field) {
    const bool maybeBool = field && field->serializedBitCount == 1;
    switch (elemTypeHash) {
    case TypeHash::DT_BYTE:
        return maybeBool ? 0 : 1;
    case TypeHash::DT_WORD:
        return maybeBool ? 0 : 2;
    case TypeHash::DT_INT:
        return maybeBool ? 0 : 4;
    case TypeHash::DT_ENUM:
        return 4;
    case TypeHash::DT_UINT:
        return maybeBool ? 0 : 4;
    case TypeHash::DT_FLOAT:
        return 4;
    case TypeHash::DT_VECTOR2D:
        return 8;
    case TypeHash::DT_VECTOR3D:
        return 12;
    case TypeHash::DT_VECTOR4D:
        return 16;
    case TypeHash::DT_BCVEC2I:
        return 8;
    case TypeHash::DT_RGBACOLOR:
        return 4;
    case TypeHash::DT_RGBACOLORVALUE:
        return 16;
    default:
        return 0;
    }
}

/// Bulk-read `count` elements of a homogeneous basic type into a SnoArray.
static SnoArray readTypedArrayFromBuf(BinaryReader& reader, size_t off, size_t count,
                                      u32 elemTypeHash) {
    reader.setPosition(static_cast<u32>(off));
    switch (elemTypeHash) {
    case TypeHash::DT_BYTE:
        return SnoArray(reader.read<std::vector<u8>>(count));
    case TypeHash::DT_WORD:
        return SnoArray(reader.read<std::vector<u16>>(count));
    case TypeHash::DT_INT:
    case TypeHash::DT_ENUM:
        return SnoArray(reader.read<std::vector<i32>>(count));
    case TypeHash::DT_UINT:
        return SnoArray(reader.read<std::vector<u32>>(count));
    case TypeHash::DT_FLOAT:
        return SnoArray(reader.read<std::vector<f32>>(count));
    case TypeHash::DT_VECTOR2D:
        return SnoArray(reader.read<std::vector<SnoVec2>>(count));
    case TypeHash::DT_VECTOR3D:
        return SnoArray(reader.read<std::vector<SnoVec3>>(count));
    case TypeHash::DT_VECTOR4D:
        return SnoArray(reader.read<std::vector<SnoVec4>>(count));
    case TypeHash::DT_BCVEC2I:
        return SnoArray(reader.read<std::vector<SnoIVec2>>(count));
    case TypeHash::DT_RGBACOLOR:
        return SnoArray(reader.read<std::vector<SnoColor>>(count));
    case TypeHash::DT_RGBACOLORVALUE:
        return SnoArray(reader.read<std::vector<SnoColorF>>(count));
    default:
        return SnoArray();
    }
}

// ============================================================================
// Helpers for common reader patterns
// ============================================================================

static SnoValue emptyArray() {
    return SnoValue(SnoArray(std::vector<SnoValue>{}));
}

static SnoValue makeExternalMarker(i32 dataOffset, i32 dataSize, i32 dataCount = -1) {
    SnoObject ext;
    ext["__external__"] = SnoValue(true);
    ext["dataOffset"] = SnoValue(dataOffset);
    ext["dataSize"] = SnoValue(dataSize);
    if (dataCount >= 0)
        ext["dataCount"] = SnoValue(dataCount);
    return SnoValue(std::move(ext));
}

template <typename T>
static SnoValue readIntValue(BinaryReader& reader, size_t offset, const SnoFieldDef* field) {
    T v = readAt<T>(reader, offset);
    if (field && field->serializedBitCount == 1)
        return SnoValue(static_cast<bool>(v));
    return SnoValue(v);
}

static SnoArray readElementLoop(const ReadCtx& parentCtx, const u32 sub[3], size_t off,
                                i32 remaining, const SnoFieldDef* field) {
    std::vector<SnoValue> arr;
    while (remaining > 0) {
        ReadCtx subCtx = parentCtx.sub();
        arr.push_back(readStructure(subCtx, sub, off, field));
        if (subCtx.readLength < 1)
            break;
        off += subCtx.readLength;
        remaining -= static_cast<i32>(subCtx.readLength);
    }
    return SnoArray(std::move(arr));
}

static SnoArray readPolyElements(const ReadCtx& parentCtx, u32 baseTypeHash, size_t off,
                                 i32 remaining, i32 count, const SnoFieldDef* field) {
    constexpr u32 kPolyBaseHash = 0x5d4bac71;
    std::vector<SnoValue> arr(static_cast<size_t>(count));
    for (i32 i = 0; i < count && remaining > 0; ++i) {
        u32 polyTypeHash = baseTypeHash;
        if (parentCtx.reg.findType(kPolyBaseHash) && off + 4 <= parentCtx.payloadSize) {
            u32 const dwType = readAt<u32>(parentCtx.reader, off);
            if (dwType != 0)
                polyTypeHash = dwType;
        }
        u32 polySub[3] = {polyTypeHash, baseTypeHash, TypeHash::DT_NULL};
        ReadCtx subCtx = parentCtx.sub();
        arr[i] = readStructure(subCtx, polySub, off, field);
        if (subCtx.readLength < 1)
            break;
        off += subCtx.readLength;
        remaining -= static_cast<i32>(subCtx.readLength);
    }
    return SnoArray(std::move(arr));
}

/// Read variable array elements, trying a bulk typed read first.
static SnoValue readVarArray(const ReadCtx& arrCtx, const u32 sub[3], size_t off, i32 dataSize,
                             const SnoFieldDef* field) {
    size_t const elemSz = typedArrayElemSize(sub[0], field);
    if (elemSz > 0) {
        size_t const count = static_cast<size_t>(dataSize) / elemSz;
        return SnoValue(readTypedArrayFromBuf(arrCtx.reader, off, count, sub[0]));
    }
    return SnoValue(readElementLoop(arrCtx, sub, off, dataSize, field));
}

// ============================================================================
// Basic type readers
// ============================================================================

static SnoValue readBasicType(ReadCtx& ctx, u32 typeHash, const u32 typeHashes[3], size_t offset,
                              const SnoFieldDef* field) {
    auto& reader = ctx.reader;

    switch (typeHash) {

    // -- Scalar integers (with optional bool promotion) --
    case TypeHash::DT_BYTE:
        return readInt<u8>(ctx, offset, field);
    case TypeHash::DT_WORD:
        return readInt<u16>(ctx, offset, field);
    case TypeHash::DT_INT:
        return readInt<i32>(ctx, offset, field);
    case TypeHash::DT_UINT:
        return readInt<u32>(ctx, offset, field);
    case TypeHash::DT_INT64:
        return readInt<i64>(ctx, offset, field);

    // -- Simple fixed-size scalars --
    case TypeHash::DT_ENUM:
        return readSimple<i32>(ctx, offset);
    case TypeHash::DT_FLOAT:
        return readSimple<f32>(ctx, offset);
    case TypeHash::DT_STARTLOC_NAME:
        return readSimple<u32>(ctx, offset);
    case TypeHash::DT_ACD_NETWORK_NAME:
    case TypeHash::DT_SHARED_SERVER_DATA_ID:
        return readSimple<u64>(ctx, offset);

    // -- DT_SNO --
    case TypeHash::DT_SNO: {
        ctx.readLength += 4;
        i32 const raw = readAt<i32>(reader, offset);
        if (raw == -1 || raw == 0 || raw == static_cast<i32>(0xFFFFFFFF))
            return SnoValue();
        return SnoValue(SnoRef{field ? field->group : -1, raw});
    }

    // -- DT_SNO_NAME --
    case TypeHash::DT_SNO_NAME: {
        ctx.readLength += 8;
        reader.setPosition(static_cast<u32>(offset));
        i32 group = reader.read<i32>();
        i32 const raw = reader.read<i32>();
        if (raw == -1 || raw == 0 || raw == static_cast<i32>(0xFFFFFFFF))
            return SnoValue();
        if (group == 0 && field)
            group = field->group;
        return SnoValue(SnoRef{group, raw});
    }

    // -- DT_GBID --
    case TypeHash::DT_GBID: {
        ctx.readLength += 4;
        u32 const raw = readAt<u32>(reader, offset);
        if (raw == 0xFFFFFFFF)
            return SnoValue();
        return SnoValue(SnoGbid{field ? field->group : -1, raw});
    }

    // -- DT_CSTRING --
    case TypeHash::DT_CSTRING: {
        ctx.readLength += 16;
        reader.setPosition(static_cast<u32>(offset + 8));
        i32 const stringOffset = reader.read<i32>();
        i32 const stringSize = reader.read<i32>();
        if (stringSize < 1 || stringOffset < 1 ||
            static_cast<size_t>(stringOffset + stringSize) > ctx.payloadSize)
            return SnoValue(std::string{});
        reader.setPosition(static_cast<u32>(stringOffset));
        return SnoValue(reader.readString(static_cast<size_t>(stringSize), true));
    }

    // -- DT_CHARARRAY --
    case TypeHash::DT_CHARARRAY: {
        i32 const arrLen = field ? field->arrayLength : 0;
        if (arrLen <= 0)
            return SnoValue(std::string{});
        ctx.readLength += static_cast<size_t>(arrLen);
        reader.setPosition(static_cast<u32>(offset));
        return SnoValue(reader.readString(static_cast<size_t>(arrLen), true));
    }

    // -- DT_STRING_FORMULA --
    case TypeHash::DT_STRING_FORMULA: {
        ctx.readLength += 32;
        reader.setPosition(static_cast<u32>(offset + 8));
        i32 const formulaOffset = reader.read<i32>();
        i32 const formulaSize = reader.read<i32>();
        if (formulaSize <= 0 || formulaOffset <= 0 ||
            static_cast<size_t>(formulaOffset + formulaSize) > ctx.payloadSize)
            return SnoValue(std::string{});
        reader.setPosition(static_cast<u32>(formulaOffset));
        return SnoValue(reader.readString(static_cast<size_t>(formulaSize), true));
    }

    // -- Geometric / color types --
    case TypeHash::DT_RGBACOLOR:
        return readSimple<SnoColor>(ctx, offset);
    case TypeHash::DT_RGBACOLORVALUE:
        return readSimple<SnoColorF>(ctx, offset);
    case TypeHash::DT_BCVEC2I:
        return readSimple<SnoIVec2>(ctx, offset);
    case TypeHash::DT_VECTOR2D:
        return readSimple<SnoVec2>(ctx, offset);
    case TypeHash::DT_VECTOR3D:
        return readSimple<SnoVec3>(ctx, offset);
    case TypeHash::DT_VECTOR4D:
        return readSimple<SnoVec4>(ctx, offset);

    // -- DT_OPTIONAL --
    case TypeHash::DT_OPTIONAL: {
        u32 sub[3];
        subTypeHashes(typeHashes, sub);
        ReadCtx subCtx = ctx.sub();
        SnoValue value = readStructure(subCtx, sub, offset, field);
        i32 const present = readAt<i32>(reader, offset);
        ctx.readLength += 4;
        return present ? std::move(value) : SnoValue();
    }

    // -- DT_RANGE --
    case TypeHash::DT_RANGE: {
        u32 sub[3];
        subTypeHashes(typeHashes, sub);
        ReadCtx subCtx = ctx.sub();
        SnoValue v1 = readStructure(subCtx, sub, offset, field);
        SnoValue v2 = readStructure(subCtx, sub, offset + subCtx.readLength, field);
        ctx.readLength += subCtx.readLength;
        SnoObject range;
        range["rangeValue1"] = std::move(v1);
        range["rangeValue2"] = std::move(v2);
        return SnoValue(std::move(range));
    }

    // -- DT_FIXEDARRAY --
    case TypeHash::DT_FIXEDARRAY: {
        i32 const arrLen = field ? field->arrayLength : 0;
        if (arrLen <= 0)
            return emptyArray();
        u32 sub[3];
        subTypeHashes(typeHashes, sub);

        // D3: fixed byte arrays are null-terminated strings
        if (ctx.format == SnoFormat::D3 && sub[0] == TypeHash::DT_BYTE) {
            size_t const len = static_cast<size_t>(arrLen);
            ctx.readLength += len;
            reader.setPosition(static_cast<u32>(offset));
            return SnoValue(reader.readString(len, true));
        }

        // Try typed array for homogeneous basic types
        size_t const elemSz = typedArrayElemSize(sub[0], field);
        if (elemSz > 0) {
            size_t const n = static_cast<size_t>(arrLen);
            if (offset + n * elemSz <= ctx.payloadSize) {
                ctx.readLength += n * elemSz;
                return SnoValue(readTypedArrayFromBuf(reader, offset, n, sub[0]));
            }
        }

        std::vector<SnoValue> arr;
        arr.reserve(static_cast<size_t>(arrLen));
        ReadCtx subCtx = ctx.sub();
        for (i32 i = 0; i < arrLen; ++i) {
            arr.push_back(readStructure(subCtx, sub, offset + subCtx.readLength, field));
        }
        ctx.readLength += subCtx.readLength;
        return SnoValue(SnoArray(std::move(arr)));
    }

    // -- DT_VARIABLEARRAY --
    case TypeHash::DT_VARIABLEARRAY: {
        ctx.readLength += 16;
        // D3 layout: {dataOffset(4), dataSize(4), 0(4), 0(4)}
        // D4 layout: {0(4), 0(4), dataOffset(4), dataSize(4)}
        size_t const descOff = (ctx.format == SnoFormat::D3) ? offset : offset + 8;
        reader.setPosition(static_cast<u32>(descOff));
        i32 const dataOffset = reader.read<i32>();
        i32 const dataSize = reader.read<i32>();

        u32 sub[3];
        subTypeHashes(typeHashes, sub);

        if (isExternalField(field)) {
            if (ctx.payloadDataReader && dataSize > 0 && dataOffset >= 0 &&
                static_cast<size_t>(dataOffset + dataSize) <= ctx.payloadDataSize)
                return readVarArray(ctx.forExternalPayload(), sub, static_cast<size_t>(dataOffset),
                                    dataSize, field);
            return makeExternalMarker(dataOffset, dataSize);
        }

        if (dataSize < 1 || dataOffset < 1 ||
            static_cast<size_t>(dataOffset + dataSize) > ctx.payloadSize)
            return emptyArray();

        return readVarArray(ctx, sub, static_cast<size_t>(dataOffset), dataSize, field);
    }

    // -- DT_POLYMORPHIC_VARIABLEARRAY --
    case TypeHash::DT_POLYMORPHIC_VARIABLEARRAY: {
        ctx.readLength += 24;
        reader.setPosition(static_cast<u32>(offset + 8));
        i32 const dataOffset = reader.read<i32>();
        i32 const dataSize = reader.read<i32>();
        i32 const dataCount = reader.read<i32>();

        if (isExternalField(field)) {
            if (ctx.payloadDataReader && dataSize > 0 && dataCount > 0 && dataOffset >= 0 &&
                static_cast<size_t>(dataOffset + dataSize) <= ctx.payloadDataSize) {
                size_t const off =
                    static_cast<size_t>(dataOffset) + static_cast<size_t>(dataCount) * 8;
                i32 const remaining = dataSize - dataCount * 8;
                return SnoValue(readPolyElements(ctx.forExternalPayload(), typeHashes[1], off,
                                                 remaining, dataCount, field));
            }
            return makeExternalMarker(dataOffset, dataSize, dataCount);
        }

        if (dataSize < 1 || dataCount < 1 || dataOffset < 1 ||
            static_cast<size_t>(dataOffset + dataSize) > ctx.payloadSize)
            return emptyArray();

        size_t const off = static_cast<size_t>(dataOffset) + static_cast<size_t>(dataCount) * 8;
        i32 const remaining = dataSize - dataCount * 8;
        return SnoValue(readPolyElements(ctx, typeHashes[1], off, remaining, dataCount, field));
    }

    // -- DT_TAGMAP --
    case TypeHash::DT_TAGMAP: {
        ctx.readLength += 16;
        // D3 layout: {dataOffset(4), dataSize(4), 0(4), 0(4)}
        // D4 layout: {0(4), 0(4), dataOffset(4), dataSize(4)}
        size_t const tmDescOff = (ctx.format == SnoFormat::D3) ? offset : offset + 8;
        reader.setPosition(static_cast<u32>(tmDescOff));
        i32 const dataOffset = reader.read<i32>();
        i32 const dataSize = reader.read<i32>();

        if (dataSize < 1 || dataOffset < 1)
            return SnoValue(SnoObject{});
        if (static_cast<size_t>(dataOffset + dataSize) > ctx.payloadSize)
            return SnoValue(SnoObject{});

        size_t doff = static_cast<size_t>(dataOffset);
        [[maybe_unused]] i32 drem = dataSize;

        i32 const dataCount = readAt<i32>(reader, doff);
        doff += 4;
        drem -= 4;

        // Step 1: read field info (names and types)
        struct TagField {
            u32 nameHash;
            u32 typeHashes[3];
            std::string name;
        };
        std::vector<TagField> tagFields(static_cast<size_t>(dataCount));
        for (i32 i = 0; i < dataCount; ++i) {
            reader.setPosition(static_cast<u32>(doff));
            tagFields[i].nameHash = reader.read<u32>();
            tagFields[i].typeHashes[0] = reader.read<u32>();
            tagFields[i].typeHashes[1] = TypeHash::DT_NULL;
            tagFields[i].typeHashes[2] = TypeHash::DT_NULL;
            doff += 8;
            drem -= 8;

            // Read additional type hashes if flagged
            for (int ti = 0; ti < 2; ++ti) {
                auto* ft = ctx.reg.findType(tagFields[i].typeHashes[ti]);
                if (!ft || !(ft->flags & 0x8000))
                    break;
                if (ti >= 1 && tagFields[i].typeHashes[0] == TypeHash::DT_BINDABLEPROPERTY &&
                    tagFields[i].typeHashes[1] == TypeHash::DT_CSTRING)
                    break;
                tagFields[i].typeHashes[ti + 1] = reader.read<u32>();
                doff += 4;
                drem -= 4;
            }

            // Resolve field name by hash — we just store the hash as a hex string
            // (the full name table would require attributes.json)
            char nameBuf[32];
            std::snprintf(nameBuf, sizeof(nameBuf), "field_%08x", tagFields[i].nameHash);
            tagFields[i].name = nameBuf;
        }

        // Step 2: read values
        SnoObject obj;
        for (i32 i = 0; i < dataCount; ++i) {
            size_t const reqAlign = getTypeAlignment(ctx.reg, tagFields[i].typeHashes, true);
            size_t const curAlign = doff % reqAlign;
            if (curAlign) {
                size_t const padding = reqAlign - curAlign;
                doff += padding;
                drem -= static_cast<i32>(padding);
            }

            SnoFieldDef tmField{};
            std::memcpy(tmField.typeHashes, tagFields[i].typeHashes, sizeof(tmField.typeHashes));
            tmField.nameIndex = 0; // not used directly
            tmField.flags = 0;
            tmField.offset = 0;
            tmField.arrayLength = -1;
            tmField.group = -1;
            tmField.serializedBitCount = 0;

            ReadCtx subCtx = ctx.sub();
            SnoValue val = readStructure(subCtx, tagFields[i].typeHashes, doff, &tmField);
            if (subCtx.readLength < 1)
                break;
            obj[tagFields[i].name] = std::move(val);
            doff += subCtx.readLength;
            drem -= static_cast<i32>(subCtx.readLength);
        }
        return SnoValue(std::move(obj));
    }

    // -- DT_BINDABLEPROPERTY --
    case TypeHash::DT_BINDABLEPROPERTY: {
        // DT_CSTRING DataStore @0, DT_CSTRING DataPath @16, DT_UINT FormatterId @32...
        SnoObject obj;

        u32 cstringHash[3] = {TypeHash::DT_CSTRING, TypeHash::DT_NULL, TypeHash::DT_NULL};
        ReadCtx sub1 = ctx.sub();
        obj["DataStore"] = readStructure(sub1, cstringHash, offset, field);

        ReadCtx sub2 = ctx.sub();
        obj["DataPath"] = readStructure(sub2, cstringHash, offset + 16, field);

        obj["FormatterId"] = SnoValue(readAt<u32>(reader, offset + 32));

        // padding + PropertyFlags + padding2 = 12 bytes
        // Then the sub-value
        size_t const subOffset = offset + 48;
        u32 sub[3] = {typeHashes[1], typeHashes[2], TypeHash::DT_NULL};
        ReadCtx sub3 = ctx.sub();
        obj["value"] = readStructure(sub3, sub, subOffset, field);

        ctx.readLength += 48 + sub3.readLength;
        if ((subOffset + sub3.readLength) % 8) {
            ctx.readLength += 4; // padding
        }
        return SnoValue(std::move(obj));
    }

    // -- DT_NULL --
    case TypeHash::DT_NULL:
    default:
        return SnoValue();
    }
}

// ============================================================================
// readStructure — the recursive dispatcher
// ============================================================================

static SnoValue readStructure(ReadCtx& ctx, const u32 typeHashes[3], size_t offset,
                              const SnoFieldDef* field) {
    if (typeHashes[0] == TypeHash::DT_NULL)
        return SnoValue();

    auto* typeDef = ctx.reg.findType(typeHashes[0]);

    // If the registry doesn't know this type (e.g. basic types in D3 registry),
    // try dispatching as a basic type directly by hash.
    if (!typeDef)
        return readBasicType(ctx, typeHashes[0], typeHashes, offset, field);

    // Basic type
    if (typeDef->isBasic) {
        return readBasicType(ctx, typeDef->hash, typeHashes, offset, field);
    }

    // Complex type — read as an object with named fields
    SnoObject obj;
    auto fields = ctx.reg.fields(*typeDef);
    for (auto& f : fields) {
        size_t fieldOff = offset + static_cast<size_t>(f.offset);

        // D3: pointer-to-struct indirection (flags & 1)
        if (ctx.format == SnoFormat::D3 && (f.flags & 1)) {
            auto* subType = ctx.reg.findType(f.typeHashes[0]);
            if (!subType || subType->isBasic || subType->size == 0)
                continue;
            if (fieldOff + 4 > ctx.payloadSize)
                continue;
            i32 const ptrOff = readAt<i32>(ctx.reader, fieldOff);
            if (ptrOff <= 0 || static_cast<size_t>(ptrOff) + subType->size > ctx.payloadSize)
                continue;
            fieldOff = static_cast<size_t>(ptrOff);
        }

        ReadCtx subCtx = ctx.sub();
        SnoValue val = readStructure(subCtx, f.typeHashes, fieldOff, &f);
        const char* name = ctx.reg.fieldName(f);
        obj[name] = std::move(val);
    }
    ctx.readLength += typeDef->size;
    return SnoValue(std::move(obj));
}

// ============================================================================
// SnoReader
// ============================================================================

SnoReader::SnoReader()
    : m_registry(d4::SnoTypeRegistry::instance()), m_d3Registry(d3::SnoTypeRegistry::instance()) {}

void SnoReader::setFormatHashes(const std::unordered_map<i32, u32>& hashes) {
    m_groupFormatHashes = hashes;
}

std::optional<SnoFile> SnoReader::parse(std::span<const u8> data) const {
    return parse(data, SnoGroup::None);
}

std::optional<SnoFile> SnoReader::parse(std::span<const u8> data, SnoGroup group) const {
    return parse(data, group, {});
}

std::optional<SnoFile> SnoReader::parse(std::span<const u8> data, SnoGroup group,
                                        std::span<const u8> payloadData) const {
    if (data.size() < 20)
        return std::nullopt;

    span_streambuf dataBuf(data);
    std::istream dataStream(&dataBuf);
    BinaryReader dataReader(dataStream);

    u32 const magic = dataReader.read<u32>();
    if (magic != kSnoMagic)
        return std::nullopt;

    u32 formatHash = dataReader.read<u32>();

    if (formatHash == 0 && group != SnoGroup::None) {
        auto it = m_groupFormatHashes.find(static_cast<i32>(group));
        if (it != m_groupFormatHashes.end())
            formatHash = it->second;
    }

    // Look up the root type via D4 registry
    u32 rootTypeHash = 0;
    if (formatHash != 0)
        rootTypeHash = m_registry.typeHashFromKey(formatHash);

    // If D4 format-hash lookup failed and the D3 registry has an explicit
    // group mapping, prefer D3 over the D4 name-based fallback.  D3 SNO
    // files store a version number (small integer) in the same field that
    // D4 uses for the format hash, so the D4 key lookup correctly fails.
    // Without this check, the name-based fallback below would find the D4
    // type (e.g. "AnimationDefinition") and parse the D3 file incorrectly.
    if (rootTypeHash == 0 && group != SnoGroup::None) {
        u32 const d3TypeHash = m_d3Registry.typeHashFromKey(static_cast<u32>(group));
        if (d3TypeHash != 0)
            return parseD3(data, group);
    }

    // If the format hash map doesn't contain this hash (e.g. game was
    // patched and format hashes changed), try to resolve the root type
    // by constructing the expected type name from the SNO group name.
    if (rootTypeHash == 0 && formatHash != 0 && group != SnoGroup::None) {
        const char* gn = snoGroupName(group);
        if (gn) {
            std::string const typeName = std::string(gn) + "Definition";
            rootTypeHash = m_registry.typeHashFromName(typeName.c_str());
        }
    }

    // If D4 lookup also failed, try D3 as last resort
    if (rootTypeHash == 0 && group != SnoGroup::None) {
        return parseD3(data, group);
    }
    if (rootTypeHash == 0)
        return std::nullopt;

    return parseD4(data, magic, formatHash, rootTypeHash, payloadData);
}

std::optional<SnoFile> SnoReader::parseD4(std::span<const u8> data, u32 magic, u32 formatHash,
                                          u32 rootTypeHash, std::span<const u8> payloadData) const {
    auto* rootType = m_registry.findType(rootTypeHash);
    if (!rootType)
        return std::nullopt;

    auto payload = data.subspan(16);
    span_streambuf payloadBuf(payload);
    std::istream payloadStream(&payloadBuf);
    BinaryReader reader(payloadStream);

    i32 const snoId = reader.read<i32>();

    // Set up external payload data reader if provided
    span_streambuf pdBuf(payloadData);
    std::istream pdStream(&pdBuf);
    BinaryReader pdReader(pdStream);
    BinaryReader* pdPtr = payloadData.empty() ? nullptr : &pdReader;

    ReadCtx ctx{reader, pdPtr, m_registry, SnoFormat::D4, payload.size(), payloadData.size(), 0};
    SnoValue root = readStructure(ctx, &rootTypeHash, 0, nullptr);

    SnoFile result;
    result.dwSignature = magic;
    result.formatHash = formatHash;
    result.snoId = snoId;
    result.typeName = m_registry.typeName(*rootType);
    result.root = std::move(root);
    return result;
}

// ============================================================================
// D3 SNO parsing
// ============================================================================

std::optional<SnoFile> SnoReader::parseD3(std::span<const u8> data, SnoGroup group) const {
    if (data.size() < 32)
        return std::nullopt;

    span_streambuf dataBuf(data);
    std::istream dataStream(&dataBuf);
    BinaryReader dataReader(dataStream);

    u32 const magic = dataReader.read<u32>();
    if (magic != kSnoMagic)
        return std::nullopt;

    u32 const version = dataReader.read<u32>();

    u32 const groupId = static_cast<u32>(group);
    u32 const rootTypeHash = m_d3Registry.typeHashFromKey(groupId);
    if (rootTypeHash == 0)
        return std::nullopt;

    auto* rootType = m_d3Registry.findType(rootTypeHash);
    if (!rootType)
        return std::nullopt;

    auto payload = data.subspan(16);
    span_streambuf payloadBuf(payload);
    std::istream payloadStream(&payloadBuf);
    BinaryReader reader(payloadStream);

    i32 const snoId = reader.read<i32>();

    ReadCtx ctx{reader, nullptr, m_d3Registry, SnoFormat::D3, payload.size(), 0, 0};
    SnoValue root = readStructure(ctx, &rootTypeHash, 0, nullptr);

    SnoFile result;
    result.dwSignature = magic;
    result.formatHash = version;
    result.snoId = snoId;
    result.typeName = m_d3Registry.typeName(*rootType);
    result.root = std::move(root);
    return result;
}

} // namespace sno
} // namespace whiteout
