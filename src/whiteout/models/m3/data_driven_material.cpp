// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/m3/structures.h>

#include <algorithm>
#include <cstring>

namespace whiteout {
namespace m3 {
namespace {

struct DataDrivenNameEntry {
    u32 hash;
    const char* name;
};

#include "data_driven_names.inl"

template <typename T>
bool readAt(const std::vector<u8>& blob, u64 offset, T& out) {
    if (offset + sizeof(T) > blob.size()) {
        return false;
    }
    std::memcpy(&out, blob.data() + offset, sizeof(T));
    return true;
}

// A property slot is live only when its type byte is 1. `count` is the array's
// capacity, not its population, and the writer leaves the tail uninitialised —
// stale slots carry plausible-looking hashes and offsets near 2^63.
constexpr u8 kLiveSlot = 1;

// Guards against a corrupt header turning into a multi-gigabyte allocation.
constexpr u32 kMaxCount = 4096;

bool decodeGroup(const std::vector<u8>& blob, u64 offset, DataDrivenGroup& group) {
    u32 count = 0;
    u64 hashes = 0, sizes = 0, types = 0, values = 0;
    if (!readAt(blob, offset, count) || !readAt(blob, offset + 4, hashes) ||
        !readAt(blob, offset + 12, sizes) || !readAt(blob, offset + 20, types) ||
        !readAt(blob, offset + 28, values)) {
        return false;
    }
    if (count == 0) {
        return hashes == 0 && sizes == 0 && types == 0 && values == 0;
    }
    if (count > kMaxCount || hashes != offset + 36 || sizes != hashes + 4ull * count ||
        types != sizes + 2ull * count || values != types + count ||
        values + 8ull * count > blob.size()) {
        return false;
    }

    group.properties.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        if (blob[static_cast<std::size_t>(types) + i] != kLiveSlot) {
            continue;
        }
        u32 hash = 0;
        u16 size = 0;
        u64 valueOffset = 0;
        readAt(blob, hashes + 4ull * i, hash);
        readAt(blob, sizes + 2ull * i, size);
        readAt(blob, values + 8ull * i, valueOffset);
        if (valueOffset + size > blob.size()) {
            continue; // stale slot that kept a live type byte
        }
        DataDrivenProperty property;
        property.nameHash = hash;
        if (const char* name = dataDrivenName(hash)) {
            property.name = name;
        }
        property.data.assign(blob.begin() + static_cast<std::ptrdiff_t>(valueOffset),
                             blob.begin() + static_cast<std::ptrdiff_t>(valueOffset + size));
        group.properties.push_back(std::move(property));
    }
    return true;
}

} // namespace

const char* dataDrivenName(u32 hash) {
    const auto* first = std::begin(kDataDrivenNames);
    const auto* last = std::end(kDataDrivenNames);
    const auto* it = std::lower_bound(
        first, last, hash,
        [](const DataDrivenNameEntry& entry, u32 value) { return entry.hash < value; });
    return (it != last && it->hash == hash) ? it->name : nullptr;
}

DataDrivenProperties DataDrivenMaterial::decodeProperties() const {
    DataDrivenProperties out;
    u32 count = 0;
    u64 keys = 0, groupOffsets = 0;
    if (!readAt(propertyBlob, 0, count) || !readAt(propertyBlob, 4, keys) ||
        !readAt(propertyBlob, 12, groupOffsets)) {
        return out;
    }
    if (count == 0 || count > kMaxCount || keys != 20 ||
        groupOffsets != 20ull + 4ull * count || groupOffsets + 8ull * count > propertyBlob.size()) {
        return out;
    }

    out.groups.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u32 hash = 0;
        u64 offset = 0;
        readAt(propertyBlob, keys + 4ull * i, hash);
        readAt(propertyBlob, groupOffsets + 8ull * i, offset);

        DataDrivenGroup group;
        group.nameHash = hash;
        if (const char* name = dataDrivenName(hash)) {
            group.name = name;
        }
        if (!decodeGroup(propertyBlob, offset, group)) {
            return {};
        }
        out.groups.push_back(std::move(group));
    }
    return out;
}

} // namespace m3
} // namespace whiteout
