// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "het_bet.h"

#include "../../common/jenkins.h"
#include "../crypto.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace whiteout::storages::mpq {

namespace {

constexpr size_t kExtHeaderSize = 12;
constexpr size_t kHetHeaderSize = 32;
constexpr size_t kBetHeaderSize = 76;
constexpr u32 kNameHashBits = 64;
/// The BET header's third field. StormLib writes 0x10; every table the
/// StarCraft II editor has written holds this, so a rebuilt one does too.
constexpr u32 kBetUnknown08 = 0x1ED0;

/// Bits needed to hold @p value (0 for 0).
u32 bitCount(u64 value) {
    u32 bits = 0;
    while (value != 0) {
        value >>= 1;
        ++bits;
    }
    return bits;
}

u64 readBits(std::span<const u8> data, u64 start, u32 count) {
    u64 value = 0;
    for (u32 i = 0; i < count; ++i) {
        u64 const bit = start + i;
        if ((data[bit / 8] >> (bit % 8)) & 1u)
            value |= u64{1} << i;
    }
    return value;
}

void writeBits(std::span<u8> data, u64 start, u32 count, u64 value) {
    for (u32 i = 0; i < count; ++i) {
        u64 const bit = start + i;
        u8 const mask = static_cast<u8>(1u << (bit % 8));
        if ((value >> i) & 1u)
            data[bit / 8] |= mask;
        else
            data[bit / 8] &= static_cast<u8>(~mask);
    }
}

u32 readU32(std::span<const u8> data, size_t offset) {
    u32 v = 0;
    std::memcpy(&v, data.data() + offset, 4);
    return v;
}

void writeU32(std::vector<u8>& data, size_t offset, u32 v) {
    std::memcpy(data.data() + offset, &v, 4);
}

/// The table past its extended header, decrypted; nothing when the header is
/// not @p magic or overruns @p stored.
std::optional<std::vector<u8>> openTable(std::span<const u8> stored, u32 magic, const char* keyName) {
    if (stored.size() < kExtHeaderSize || readU32(stored, 0) != magic)
        return std::nullopt;
    u32 const dataSize = readU32(stored, 8);
    if (kExtHeaderSize + dataSize > stored.size())
        return std::nullopt;
    std::vector<u8> body(stored.begin() + kExtHeaderSize, stored.begin() + kExtHeaderSize + dataSize);
    std::vector<u32> words(body.size() / 4);
    std::memcpy(words.data(), body.data(), words.size() * 4);
    decryptBlock(words.data(), words.size(), hashString(keyName, HashType::FileKey));
    std::memcpy(body.data(), words.data(), words.size() * 4);
    return body;
}

/// @p body behind its extended header, encrypted the way @ref openTable reads it.
std::vector<u8> sealTable(std::vector<u8> body, u32 magic, const char* keyName) {
    std::vector<u32> words(body.size() / 4);
    std::memcpy(words.data(), body.data(), words.size() * 4);
    encryptBlock(words.data(), words.size(), hashString(keyName, HashType::FileKey));
    std::memcpy(body.data(), words.data(), words.size() * 4);

    std::vector<u8> stored(kExtHeaderSize + body.size());
    writeU32(stored, 0, magic);
    writeU32(stored, 4, 1);
    writeU32(stored, 8, static_cast<u32>(body.size()));
    std::memcpy(stored.data() + kExtHeaderSize, body.data(), body.size());
    return stored;
}

} // anonymous namespace

u64 hetNameHash(const std::string& filename) {
    std::string normalized;
    normalized.reserve(filename.size());
    for (char const ch : filename)
        normalized.push_back(ch == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    u32 secondary = 2;
    u32 primary = 1;
    common::jenkinsHashlittle2(normalized.data(), normalized.size(), secondary, primary);
    return ((static_cast<u64>(primary) << 32) | secondary) | (u64{1} << (kNameHashBits - 1));
}

std::optional<HetTable> parseHetTable(std::span<const u8> stored) {
    auto body = openTable(stored, kHetMagic, "(hash table)");
    if (!body || body->size() < kHetHeaderSize)
        return std::nullopt;
    const std::span<const u8> b(*body);
    HetTable het;
    het.entryCount = readU32(b, 4);
    het.totalCount = readU32(b, 8);
    het.nameHashBitSize = readU32(b, 12);
    het.indexSizeTotal = readU32(b, 16);
    het.indexSizeExtra = readU32(b, 20);
    het.indexSize = readU32(b, 24);
    u32 const indexTableSize = readU32(b, 28);
    if (het.indexSize > 32 || het.indexSize > het.indexSizeTotal ||
        kHetHeaderSize + u64{het.totalCount} + indexTableSize > b.size() ||
        u64{het.totalCount} * het.indexSizeTotal > u64{indexTableSize} * 8)
        return std::nullopt;
    het.nameHashes1.assign(b.begin() + kHetHeaderSize, b.begin() + kHetHeaderSize + het.totalCount);
    const std::span<const u8> indexBits = b.subspan(kHetHeaderSize + het.totalCount, indexTableSize);
    het.betIndexes.resize(het.totalCount);
    for (u32 i = 0; i < het.totalCount; ++i)
        het.betIndexes[i] = static_cast<u32>(readBits(indexBits, u64{i} * het.indexSizeTotal, het.indexSize));
    return het;
}

std::optional<BetTable> parseBetTable(std::span<const u8> stored) {
    auto body = openTable(stored, kBetMagic, "(block table)");
    if (!body || body->size() < kBetHeaderSize)
        return std::nullopt;
    const std::span<const u8> b(*body);
    BetTable bet;
    bet.entryCount = readU32(b, 4);
    bet.unknown08 = readU32(b, 8);
    bet.tableEntrySize = readU32(b, 12);
    bet.bitIndexFilePos = readU32(b, 16);
    bet.bitIndexFileSize = readU32(b, 20);
    bet.bitIndexCmpSize = readU32(b, 24);
    bet.bitIndexFlagIndex = readU32(b, 28);
    bet.bitIndexUnknown = readU32(b, 32);
    bet.bitCountFilePos = readU32(b, 36);
    bet.bitCountFileSize = readU32(b, 40);
    bet.bitCountCmpSize = readU32(b, 44);
    bet.bitCountFlagIndex = readU32(b, 48);
    bet.bitCountUnknown = readU32(b, 52);
    bet.bitTotalNameHash2 = readU32(b, 56);
    bet.bitExtraNameHash2 = readU32(b, 60);
    bet.bitCountNameHash2 = readU32(b, 64);
    u32 const nameHashArraySize = readU32(b, 68);
    u32 const flagCount = readU32(b, 72);

    for (u32 const bits : {bet.bitCountFilePos, bet.bitCountFileSize, bet.bitCountCmpSize,
                           bet.bitCountFlagIndex, bet.bitCountNameHash2})
        if (bits > 64)
            return std::nullopt;
    u64 const tableBytes = (u64{bet.entryCount} * bet.tableEntrySize + 7) / 8;
    u64 const flagsEnd = kBetHeaderSize + u64{flagCount} * 4;
    if (flagsEnd + tableBytes + nameHashArraySize > b.size() ||
        u64{bet.entryCount} * bet.bitTotalNameHash2 > u64{nameHashArraySize} * 8)
        return std::nullopt;

    bet.flags.resize(flagCount);
    for (u32 i = 0; i < flagCount; ++i)
        bet.flags[i] = readU32(b, kBetHeaderSize + size_t{i} * 4);
    const std::span<const u8> table = b.subspan(flagsEnd, tableBytes);
    const std::span<const u8> hashes = b.subspan(flagsEnd + tableBytes, nameHashArraySize);
    bet.entries.resize(bet.entryCount);
    for (u32 i = 0; i < bet.entryCount; ++i) {
        u64 const base = u64{i} * bet.tableEntrySize;
        BetTable::Entry& e = bet.entries[i];
        e.filePos = readBits(table, base + bet.bitIndexFilePos, bet.bitCountFilePos);
        e.fileSize = readBits(table, base + bet.bitIndexFileSize, bet.bitCountFileSize);
        e.cmpSize = readBits(table, base + bet.bitIndexCmpSize, bet.bitCountCmpSize);
        e.flagIndex = static_cast<u32>(readBits(table, base + bet.bitIndexFlagIndex, bet.bitCountFlagIndex));
        e.nameHash2 = readBits(hashes, u64{i} * bet.bitTotalNameHash2, bet.bitCountNameHash2);
    }
    return bet;
}

std::vector<u64> blockNameHashes(const HetTable& het, const BetTable& bet) {
    std::vector<u64> out(bet.entries.size(), 0);
    u32 const lowBits = het.nameHashBitSize >= 8 ? het.nameHashBitSize - 8 : 0;
    for (u32 slot = 0; slot < het.totalCount; ++slot) {
        u32 const index = het.betIndexes[slot];
        if (het.nameHashes1[slot] == 0 || index >= out.size())
            continue;
        out[index] = (u64{het.nameHashes1[slot]} << lowBits) | bet.entries[index].nameHash2;
    }
    return out;
}

std::vector<u8> buildHetTable(std::span<const u64> nameHashes, std::span<const u32> fillOrder) {
    u32 const entryCount = static_cast<u32>(nameHashes.size());
    u32 const totalCount = std::max<u32>(entryCount * 4 / 3, entryCount);
    u32 const indexBits = bitCount(entryCount);
    u32 const indexTableSize = static_cast<u32>((u64{totalCount} * indexBits + 7) / 8);

    std::vector<u8> body(kHetHeaderSize + totalCount + indexTableSize, 0);
    writeU32(body, 0, static_cast<u32>(body.size()));
    writeU32(body, 4, entryCount);
    writeU32(body, 8, totalCount);
    writeU32(body, 12, kNameHashBits);
    writeU32(body, 16, indexBits);
    writeU32(body, 20, 0);
    writeU32(body, 24, indexBits);
    writeU32(body, 28, indexTableSize);

    const std::span<u8> slots(body.data() + kHetHeaderSize, totalCount);
    const std::span<u8> indexes(body.data() + kHetHeaderSize + totalCount, indexTableSize);
    // A free slot's index is all ones.
    std::fill(indexes.begin(), indexes.end(), u8{0xFF});
    for (u32 i = 0; i < entryCount && totalCount != 0; ++i) {
        u32 const block = i < fillOrder.size() ? fillOrder[i] : i;
        u64 const hash = nameHashes[block];
        u32 slot = static_cast<u32>(hash % totalCount);
        while (slots[slot] != 0)
            slot = (slot + 1) % totalCount;
        slots[slot] = static_cast<u8>(hash >> (kNameHashBits - 8));
        writeBits(indexes, u64{slot} * indexBits, indexBits, block);
    }
    return sealTable(std::move(body), kHetMagic, "(hash table)");
}

std::vector<u8> buildBetTable(std::span<const BlockEntry> blocks, std::span<const u64> nameHashes) {
    u32 const entryCount = static_cast<u32>(blocks.size());
    // Flags are numbered in the order they are first met.
    std::vector<u32> flags;
    std::vector<u32> flagIndex(entryCount, 0);
    std::unordered_map<u32, u32> flagSlot;
    u64 maxPos = 0, maxSize = 0, maxCmp = 0;
    for (u32 i = 0; i < entryCount; ++i) {
        const BlockEntry& be = blocks[i];
        u32 const value = static_cast<u32>(be.flags);
        auto [it, fresh] = flagSlot.try_emplace(value, static_cast<u32>(flags.size()));
        if (fresh)
            flags.push_back(value);
        flagIndex[i] = it->second;
        maxPos = std::max<u64>(maxPos, be.fileOffset);
        maxSize = std::max<u64>(maxSize, be.uncompressedSize);
        maxCmp = std::max<u64>(maxCmp, be.compressedSize);
    }

    u32 const bitsPos = bitCount(maxPos);
    u32 const bitsSize = bitCount(maxSize);
    u32 const bitsCmp = bitCount(maxCmp);
    u32 const bitsFlag = bitCount(flags.empty() ? 0 : flags.size());
    u32 const entrySize = bitsPos + bitsSize + bitsCmp + bitsFlag;
    u32 const hash2Bits = kNameHashBits - 8;
    u64 const tableBytes = (u64{entryCount} * entrySize + 7) / 8;
    u32 const hashBytes = static_cast<u32>((u64{entryCount} * hash2Bits + 7) / 8);
    size_t const flagsEnd = kBetHeaderSize + flags.size() * 4;

    std::vector<u8> body(flagsEnd + tableBytes + hashBytes, 0);
    writeU32(body, 0, static_cast<u32>(body.size()));
    writeU32(body, 4, entryCount);
    writeU32(body, 8, kBetUnknown08);
    writeU32(body, 12, entrySize);
    writeU32(body, 16, 0);
    writeU32(body, 20, bitsPos);
    writeU32(body, 24, bitsPos + bitsSize);
    writeU32(body, 28, bitsPos + bitsSize + bitsCmp);
    writeU32(body, 32, entrySize);
    writeU32(body, 36, bitsPos);
    writeU32(body, 40, bitsSize);
    writeU32(body, 44, bitsCmp);
    writeU32(body, 48, bitsFlag);
    writeU32(body, 52, 0);
    writeU32(body, 56, hash2Bits);
    writeU32(body, 60, 0);
    writeU32(body, 64, hash2Bits);
    writeU32(body, 68, hashBytes);
    writeU32(body, 72, static_cast<u32>(flags.size()));
    for (size_t i = 0; i < flags.size(); ++i)
        writeU32(body, kBetHeaderSize + i * 4, flags[i]);

    const std::span<u8> table(body.data() + flagsEnd, tableBytes);
    const std::span<u8> hashes(body.data() + flagsEnd + tableBytes, hashBytes);
    for (u32 i = 0; i < entryCount; ++i) {
        u64 const base = u64{i} * entrySize;
        writeBits(table, base, bitsPos, blocks[i].fileOffset);
        writeBits(table, base + bitsPos, bitsSize, blocks[i].uncompressedSize);
        writeBits(table, base + bitsPos + bitsSize, bitsCmp, blocks[i].compressedSize);
        writeBits(table, base + bitsPos + bitsSize + bitsCmp, bitsFlag, flagIndex[i]);
        writeBits(hashes, u64{i} * hash2Bits, hash2Bits, nameHashes[i]);
    }
    return sealTable(std::move(body), kBetMagic, "(block table)");
}

} // namespace whiteout::storages::mpq
