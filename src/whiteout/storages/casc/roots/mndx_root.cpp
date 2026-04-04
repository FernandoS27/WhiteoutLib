// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mndx_root.h"
#include "common/root_build_utils.h"
#include "../../common/byte_order.h"
#include "../../common/string_utils.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

using storages::common::readLE32;
using storages::common::readLE64;
using storages::common::normalizeCascPath;

// ============================================================================
// Constants
// ============================================================================

static constexpr u32 kMndxSignature   = 0x58444E4D; // 'MNDX'
static constexpr u32 kMarSignature    = 0x0052414D; // 'MAR\0'
static constexpr u32 kMarCount        = 3;
static constexpr u32 kMarPackageNames = 0;
static constexpr u32 kMarStrippedNames = 1;
static constexpr u32 kMarFullNames    = 2;

static constexpr u32 kMndxLastCKeyEntry = 0x80000000;
static constexpr u32 kCKeySize           = 16;
static constexpr u32 kMndxCKeyEntrySize  = 24; // Flags(4) + CKey(16) + ContentSize(4)

static constexpr u32 kSearchInitializing = 0;
static constexpr u32 kSearchSearching    = 2;
static constexpr u32 kSearchFinished     = 4;

static constexpr u32 kInvalidIndex = 0xFFFFFFFF;

static constexpr u32 kPackageIndexMask  = 0x00FFFFFF; ///< Low 24 bits of CKey flags = package index.
static constexpr u32 kSingleCharMarker  = 0xFFFFFF00; ///< Hash entry sentinel: fragment is a single char.

// Sparse array group geometry.
static constexpr u32 kSparseGroupBits = 9;                      ///< log2(group size).
static constexpr u32 kSparseGroupSize = 1u << kSparseGroupBits;  ///< 512 entries per BASEVALS group.
static constexpr u32 kSparseGroupMask = kSparseGroupSize - 1;    ///< 0x1FF.

// ============================================================================
// Bit position lookup table
// ============================================================================

/// table[rank << 8 | byte_value] = position of the rank-th set bit (0-based).
/// rank is 0..7, byte_value is 0..255.  Returns 7 as sentinel when not enough bits.
static constexpr u32 kBitPosTableSize = 8 * 256; ///< 0x800.

static u8 s_bitPosTable[kBitPosTableSize];

static void ensureBitPosTable() {
    [[maybe_unused]] static const bool s_init = [] {
        for (u32 rank = 0; rank < 8; ++rank) {
            for (u32 byteVal = 0; byteVal < 256; ++byteVal) {
                u32 count = 0;
                u8 pos = 7; // sentinel
                for (u32 bit = 0; bit < 8; ++bit) {
                    if (byteVal & (1u << bit)) {
                        if (count == rank) {
                            pos = static_cast<u8>(bit);
                            break;
                        }
                        count++;
                    }
                }
                s_bitPosTable[(rank << 8) | byteVal] = pos;
            }
        }
        return true;
    }();
}

// ============================================================================
// SetBits helper (popcount per byte)
// ============================================================================

struct SetBits {
    u8 lower08;
    u8 lower16;
    u8 lower24;
    u8 lower32;
};

static SetBits getNumberOfSetBits(u32 value) {
    value = ((value >> 1) & 0x55555555) + (value & 0x55555555);
    value = ((value >> 2) & 0x33333333) + (value & 0x33333333);
    value = ((value >> 4) & 0x0F0F0F0F) + (value & 0x0F0F0F0F);
    u32 all = value * 0x01010101;
    SetBits sb;
    sb.lower08 = static_cast<u8>(all & 0xFF);
    sb.lower16 = static_cast<u8>((all >> 8) & 0xFF);
    sb.lower24 = static_cast<u8>((all >> 16) & 0xFF);
    sb.lower32 = static_cast<u8>((all >> 24) & 0xFF);
    return sb;
}

static u32 popcount32(u32 value) {
    return getNumberOfSetBits(value).lower32;
}

// ============================================================================
// ByteStream — minimal stream reader for MAR data
// ============================================================================

class ByteStream {
public:
    ByteStream() = default;

    bool init(const u8* data, size_t size) {
        m_data = data;
        m_size = size;
        m_pos = 0;
        return true;
    }

    bool getU32(u32& out) {
        if (m_pos + 4 > m_size) return false;
        std::memcpy(&out, m_data + m_pos, 4);
        m_pos += 4;
        return true;
    }

    bool getU64(u64& out) {
        if (m_pos + 8 > m_size) return false;
        std::memcpy(&out, m_data + m_pos, 8);
        m_pos += 8;
        return true;
    }

    bool getBytes(void* dst, size_t count) {
        if (m_pos + count > m_size) return false;
        std::memcpy(dst, m_data + m_pos, count);
        m_pos += count;
        return true;
    }

    const u8* getPointer(size_t count) {
        if (m_pos + count > m_size) return nullptr;
        auto p = m_data + m_pos;
        m_pos += count;
        return p;
    }

    bool skip(size_t count) {
        if (m_pos + count > m_size) return false;
        m_pos += count;
        return true;
    }

    // Read array: 8-byte length prefix (byte count), then data, then align to 8.
    // Returns pointer to in-place data (no copy).
    template <typename T>
    bool getArray(const T*& outPtr, u32& outCount) {
        u64 byteCount;
        if (!getU64(byteCount)) return false;
        if (byteCount > 0xFFFFFFFF || (byteCount % sizeof(T)) != 0) return false;
        outCount = static_cast<u32>(byteCount / sizeof(T));
        u32 numBytes = static_cast<u32>(byteCount);
        auto ptr = getPointer(numBytes);
        if (!ptr) return false;
        outPtr = reinterpret_cast<const T*>(ptr);
        // Align to 8 bytes.
        u32 pad = (~numBytes + 1) & 0x07;
        if (pad > 0 && !skip(pad)) return false;
        return true;
    }

    size_t remaining() const { return m_size - m_pos; }

private:
    const u8* m_data = nullptr;
    size_t m_size = 0;
    size_t m_pos = 0;
};

// ============================================================================
// BASEVALS — per-0x200 shortcut for sparse array
// ============================================================================

#pragma pack(push, 4)
struct BaseVals {
    u32 baseValue200;
    // Bitfields for sub-group shortcuts (packed into 2 DWORDs).
    u32 addValue40  : 7;
    u32 addValue80  : 8;
    u32 addValueC0  : 8;
    u32 addValue100 : 9;
    u32 addValue140 : 9;
    u32 addValue180 : 9;
    u32 addValue1C0 : 9;
    u32 __padding   : 5;
};
#pragma pack(pop)

static_assert(sizeof(BaseVals) == 12, "BaseVals must be 12 bytes");

// ============================================================================
// HashEntry — trie hash table entry
// ============================================================================

struct HashEntry {
    u32 nodeIndex;
    u32 nextIndex;
    union {
        u32 fragmentOffset;
        u32 childTableIndex;
        char singleChar;
    };
};

static_assert(sizeof(HashEntry) == 12, "HashEntry must be 12 bytes");

// ============================================================================
// SparseArray — bit-indexed array with hierarchical shortcuts
// ============================================================================

class SparseArray {
public:
    bool loadFromStream(ByteStream& stream) {
        // Load ItemBits.
        if (!stream.getArray(m_itemBits, m_itemBitsCount)) return false;

        u32 total, valid;
        if (!stream.getU32(total)) return false;
        if (!stream.getU32(valid)) return false;
        if (valid > total) return false;
        m_totalItemCount = total;
        m_validItemCount = valid;

        if (!stream.getArray(m_baseVals, m_baseValsCount)) return false;
        if (!stream.getArray(m_indexToItem0, m_indexToItem0Count)) return false;
        if (!stream.getArray(m_indexToItem1, m_indexToItem1Count)) return false;
        return true;
    }

    bool isEmpty() const { return m_totalItemCount == 0; }
    size_t totalItemCount() const { return m_totalItemCount; }
    size_t validItemCount() const { return m_validItemCount; }

    bool isItemPresent(size_t index) const {
        if (index >= m_totalItemCount) return false;
        return (m_itemBits[index >> 5] & (1u << (index & 0x1F))) != 0;
    }

    u32 getItemValueAt(size_t index) const {
        auto& sv = m_baseVals[index >> kSparseGroupBits];
        u32 intValue = sv.baseValue200;

        switch (((index >> 6) & 7) - 1) {
            case 0: intValue += sv.addValue40;  break;
            case 1: intValue += sv.addValue80;  break;
            case 2: intValue += sv.addValueC0;  break;
            case 3: intValue += sv.addValue100; break;
            case 4: intValue += sv.addValue140; break;
            case 5: intValue += sv.addValue180; break;
            case 6: intValue += sv.addValue1C0; break;
        }

        if (index & 0x20)
            intValue += popcount32(m_itemBits[(index >> 5) - 1]);

        u32 bitMask = (1u << (index & 0x1F)) - 1;
        return intValue + popcount32(m_itemBits[index >> 5] & bitMask);
    }

    // Get the n-th "zero" item (item whose bit is NOT set).
    u32 getItem0(u32 index) const {
        if ((index & kSparseGroupMask) == 0)
            return m_indexToItem0[index >> kSparseGroupBits];

        u32 groupIndex = findGroup0(index);
        u32 edx = index + m_baseVals[groupIndex].baseValue200 - (groupIndex << kSparseGroupBits);
        u32 dwordIndex = groupIndex << 4;

        // Navigate sub-checkpoints.
        auto& bv = m_baseVals[groupIndex];
        if (edx < 0x100 - bv.addValue100) {
            if (edx < 0x80 - bv.addValue80) {
                if (edx >= 0x40 - bv.addValue40) {
                    dwordIndex += 2;
                    edx = edx + bv.addValue40 - 0x40;
                }
            } else {
                if (edx < 0xC0 - bv.addValueC0) {
                    dwordIndex += 4;
                    edx = edx + bv.addValue80 - 0x80;
                } else {
                    dwordIndex += 6;
                    edx = edx + bv.addValueC0 - 0xC0;
                }
            }
        } else {
            if (edx < 0x180 - bv.addValue180) {
                if (edx < 0x140 - bv.addValue140) {
                    dwordIndex += 8;
                    edx = edx + bv.addValue100 - 0x100;
                } else {
                    dwordIndex += 10;
                    edx = edx + bv.addValue140 - 0x140;
                }
            } else {
                if (edx < 0x1C0 - bv.addValue1C0) {
                    dwordIndex += 12;
                    edx = edx + bv.addValue180 - 0x180;
                } else {
                    dwordIndex += 14;
                    edx = edx + bv.addValue1C0 - 0x1C0;
                }
            }
        }

        u32 bitGroup = ~m_itemBits[dwordIndex];
        SetBits zeroBits = getNumberOfSetBits(bitGroup);

        if (edx >= zeroBits.lower32) {
            bitGroup = ~m_itemBits[++dwordIndex];
            edx -= zeroBits.lower32;
            zeroBits = getNumberOfSetBits(bitGroup);
        }

        u32 itemIndex = dwordIndex << 5;

        if (edx < zeroBits.lower16) {
            if (edx >= zeroBits.lower08) {
                bitGroup >>= 8;
                itemIndex += 8;
                edx -= zeroBits.lower08;
            }
        } else {
            if (edx < zeroBits.lower24) {
                bitGroup >>= 16;
                itemIndex += 16;
                edx -= zeroBits.lower16;
            } else {
                bitGroup >>= 24;
                itemIndex += 24;
                edx -= zeroBits.lower24;
            }
        }

        edx <<= 8;
        bitGroup &= 0xFF;
        return s_bitPosTable[bitGroup + edx] + itemIndex;
    }

    // Get the n-th "one" item (item whose bit IS set).
    u32 getItem1(u32 index) const {
        if ((index & kSparseGroupMask) == 0)
            return m_indexToItem1[index >> kSparseGroupBits];

        u32 groupIndex = findGroup1(index);
        u32 distFromBase = index - m_baseVals[groupIndex].baseValue200;
        u32 dwordIndex = groupIndex << 4;

        auto& bv = m_baseVals[groupIndex];
        if (distFromBase < bv.addValue100) {
            if (distFromBase < bv.addValue80) {
                if (distFromBase >= bv.addValue40) {
                    distFromBase -= bv.addValue40;
                    dwordIndex += 2;
                }
            } else {
                if (distFromBase < bv.addValueC0) {
                    distFromBase -= bv.addValue80;
                    dwordIndex += 4;
                } else {
                    distFromBase -= bv.addValueC0;
                    dwordIndex += 6;
                }
            }
        } else {
            if (distFromBase < bv.addValue180) {
                if (distFromBase < bv.addValue140) {
                    distFromBase -= bv.addValue100;
                    dwordIndex += 8;
                } else {
                    distFromBase -= bv.addValue140;
                    dwordIndex += 10;
                }
            } else {
                if (distFromBase < bv.addValue1C0) {
                    distFromBase -= bv.addValue180;
                    dwordIndex += 12;
                } else {
                    distFromBase -= bv.addValue1C0;
                    dwordIndex += 14;
                }
            }
        }

        u32 bitGroup = m_itemBits[dwordIndex];
        SetBits setBits = getNumberOfSetBits(bitGroup);

        if (distFromBase >= setBits.lower32) {
            bitGroup = m_itemBits[++dwordIndex];
            distFromBase -= setBits.lower32;
            setBits = getNumberOfSetBits(bitGroup);
        }

        u32 itemIndex = dwordIndex << 5;

        if (distFromBase < setBits.lower16) {
            if (distFromBase >= setBits.lower08) {
                itemIndex += 8;
                bitGroup >>= 8;
                distFromBase -= setBits.lower08;
            }
        } else {
            if (distFromBase < setBits.lower24) {
                bitGroup >>= 16;
                itemIndex += 16;
                distFromBase -= setBits.lower16;
            } else {
                bitGroup >>= 24;
                itemIndex += 24;
                distFromBase -= setBits.lower24;
            }
        }

        bitGroup &= 0xFF;
        distFromBase <<= 8;
        return s_bitPosTable[bitGroup + distFromBase] + itemIndex;
    }

private:
    u32 findGroup0(u32 index) const {
        u32 minGroup = m_indexToItem0[index >> kSparseGroupBits] >> kSparseGroupBits;
        u32 maxGroup = (m_indexToItem0[(index >> kSparseGroupBits) + 1] + kSparseGroupMask) >> kSparseGroupBits;

        if ((maxGroup - minGroup) < 10) {
            while (index >= (minGroup + 1) * kSparseGroupSize - m_baseVals[minGroup + 1].baseValue200)
                minGroup++;
        } else {
            while ((minGroup + 1) < maxGroup) {
                u32 mid = (maxGroup + minGroup) >> 1;
                if (index < (maxGroup << kSparseGroupBits) - m_baseVals[maxGroup].baseValue200)
                    maxGroup = mid;
                else
                    minGroup = mid;
            }
        }
        return minGroup;
    }

    u32 findGroup1(u32 index) const {
        u32 startValue = m_indexToItem1[index >> kSparseGroupBits] >> kSparseGroupBits;
        u32 nextValue = (m_indexToItem1[(index >> kSparseGroupBits) + 1] + kSparseGroupMask) >> kSparseGroupBits;

        if ((nextValue - startValue) < 10) {
            while (index >= m_baseVals[startValue + 1].baseValue200)
                startValue++;
        } else {
            while ((startValue + 1) < nextValue) {
                u32 mid = (nextValue + startValue) >> 1;
                if (index < m_baseVals[mid].baseValue200)
                    nextValue = mid;
                else
                    startValue = mid;
            }
        }
        return startValue;
    }

    const u32* m_itemBits = nullptr;
    u32 m_itemBitsCount = 0;
    u32 m_totalItemCount = 0;
    u32 m_validItemCount = 0;
    const BaseVals* m_baseVals = nullptr;
    u32 m_baseValsCount = 0;
    const u32* m_indexToItem0 = nullptr;
    u32 m_indexToItem0Count = 0;
    const u32* m_indexToItem1 = nullptr;
    u32 m_indexToItem1Count = 0;
};

// ============================================================================
// BitEntryArray — packed bit array with variable bits-per-entry
// ============================================================================

class BitEntryArray {
public:
    bool loadFromStream(ByteStream& stream) {
        // Load base DWORD array.
        if (!stream.getArray(m_items, m_itemCount)) return false;

        if (!stream.getU32(m_bitsPerEntry)) return false;
        if (m_bitsPerEntry > 32) return false;

        if (!stream.getU32(m_entryBitMask)) return false;

        u64 val64;
        if (!stream.getU64(val64)) return false;
        if (val64 > 0xFFFFFFFF) return false;
        m_totalEntries = static_cast<u32>(val64);

        return true;
    }

    u32 getItem(u32 entryIndex) const {
        u32 dwItemIndex = (entryIndex * m_bitsPerEntry) >> 5;
        u32 dwStartBit = (entryIndex * m_bitsPerEntry) & 0x1F;
        u32 dwEndBit = dwStartBit + m_bitsPerEntry;
        u32 result;

        if (dwEndBit > 32) {
            result = (m_items[dwItemIndex + 1] << (32 - dwStartBit)) |
                     (m_items[dwItemIndex] >> dwStartBit);
        } else {
            result = m_items[dwItemIndex] >> dwStartBit;
        }
        return result & m_entryBitMask;
    }

private:
    const u32* m_items = nullptr;
    u32 m_itemCount = 0;
    u32 m_bitsPerEntry = 0;
    u32 m_entryBitMask = 0;
    u32 m_totalEntries = 0;
};

// ============================================================================
// PathFragmentTable — path fragment storage with optional mark separators
// ============================================================================

class PathFragmentTable {
public:
    bool loadFromStream(ByteStream& stream) {
        if (!stream.getArray(m_fragments, m_fragmentCount)) return false;
        if (!m_pathMarks.loadFromStream(stream)) return false;
        return true;
    }

    bool compareFragment(const char* searchMask, u32& pathLength, size_t offset) const {
        if (m_pathMarks.isEmpty()) {
            while (m_fragments[offset] == searchMask[pathLength]) {
                pathLength++;
                offset++;
                if (m_fragments[offset] == 0) return true;
                // Caller ensures pathLength < cchSearchMask before calling.
            }
            return false;
        } else {
            while (m_fragments[offset] == searchMask[pathLength]) {
                pathLength++;
                if (m_pathMarks.isItemPresent(offset++)) return true;
            }
            return false;
        }
    }

    void copyFragment(std::vector<char>& pathBuffer, size_t offset) const {
        if (m_pathMarks.isEmpty()) {
            while (m_fragments[offset] != 0)
                pathBuffer.push_back(m_fragments[offset++]);
        } else {
            while (!m_pathMarks.isItemPresent(offset))
                pathBuffer.push_back(m_fragments[offset++]);
        }
    }

    bool compareAndCopyFragment(const char* searchMask, u32 cchSearchMask,
                                std::vector<char>& pathBuffer, u32& pathLength,
                                size_t offset) const {
        if (m_pathMarks.isEmpty()) {
            while (pathLength < cchSearchMask) {
                if (m_fragments[offset] != searchMask[pathLength]) return false;
                pathBuffer.push_back(m_fragments[offset++]);
                pathLength++;
                if (m_fragments[offset] == 0) return true;
            }
            while (m_fragments[offset] != 0)
                pathBuffer.push_back(m_fragments[offset++]);
        } else {
            while (pathLength < cchSearchMask) {
                if (m_fragments[offset] != searchMask[pathLength]) return false;
                pathBuffer.push_back(m_fragments[offset]);
                pathLength++;
                if (m_pathMarks.isItemPresent(offset++)) return true;
            }
            while (!m_pathMarks.isItemPresent(offset))
                pathBuffer.push_back(m_fragments[offset++]);
        }
        return true;
    }

    bool empty() const { return m_fragmentCount == 0; }

private:
    const char* m_fragments = nullptr;
    u32 m_fragmentCount = 0;
    SparseArray m_pathMarks;
};

// ============================================================================
// PathStop — search checkpoint for trie enumeration
// ============================================================================

struct PathStop {
    u32 nodeIndex = 0;              ///< Current trie node index.
    u32 collisionPos = 0;           ///< Position in the collision table.
    u32 savedPathLen = 0;           ///< Path buffer length at this checkpoint.
    u32 hiBitsIndex = kInvalidIndex; ///< Hi-bits table index for path fragments.
    u32 fileNameIndex = kInvalidIndex; ///< Cached file name index.

    PathStop() = default;
    PathStop(u32 node, u32 colPos, u32 pathLen)
        : nodeIndex(node), collisionPos(colPos), savedPathLen(pathLen) {}
};

// ============================================================================
// SearchState — mutable state for trie search/enumeration
// ============================================================================

struct SearchState {
    // Trie walk state.
    u32 nodeIndex = 0;
    u32 pathLength = 0;
    u32 searchPhase = kSearchInitializing;
    u32 itemCount = 0;
    std::vector<PathStop> pathStops;
    std::vector<char> pathBuffer;

    // Search mask.
    const char* searchMask = nullptr;
    u32 cchSearchMask = 0;

    // Result.
    const char* foundPath = nullptr;
    u32 cchFoundPath = 0;
    u32 nIndex = 0;

    void beginSearch() {
        pathBuffer.clear();
        pathBuffer.reserve(0x40);
        pathStops.clear();
        pathStops.reserve(4);
        pathLength = 0;
        nodeIndex = 0;
        itemCount = 0;
        searchPhase = kSearchSearching;
    }

    u32 calcHashValue() const {
        return static_cast<u8>(searchMask[pathLength]) ^ (nodeIndex << 5) ^ nodeIndex;
    }
};

// ============================================================================
// FileNameDatabase — MARS trie structure for file name lookup/enumeration
// ============================================================================

class FileNameDatabase {
public:
    bool load(const u8* data, size_t size) {
        ByteStream stream;
        if (!stream.init(data, size)) return false;

        u32 sig;
        if (!stream.getU32(sig)) return false;
        if (sig != kMarSignature) return false;

        return loadFromStream(stream);
    }

    bool findFile(SearchState& search) const {
        search.nodeIndex = 0;
        search.pathLength = 0;
        search.searchPhase = kSearchInitializing;

        while (search.pathLength < search.cchSearchMask) {
            if (!comparePathFragment(search)) return false;
        }

        if (!m_fileNameIndexes.isItemPresent(search.nodeIndex)) return false;

        search.foundPath = search.searchMask;
        search.cchFoundPath = search.cchSearchMask;
        search.nIndex = m_fileNameIndexes.getItemValueAt(search.nodeIndex);
        return true;
    }

    bool doSearch(SearchState& search) const {
        switch (search.searchPhase) {
        case kSearchInitializing: {
            search.beginSearch();

            while (search.pathLength < search.cchSearchMask) {
                if (!compareAndCopyPathFragment(search)) {
                    search.searchPhase = kSearchFinished;
                    return false;
                }
            }

            search.pathStops.emplace_back(search.nodeIndex, 0,
                                          static_cast<u32>(search.pathBuffer.size()));
            search.itemCount = 1;

            if (m_fileNameIndexes.isItemPresent(search.nodeIndex)) {
                search.foundPath = search.pathBuffer.data();
                search.cchFoundPath = static_cast<u32>(search.pathBuffer.size());
                search.nIndex = m_fileNameIndexes.getItemValueAt(search.nodeIndex);
                return true;
            }
            [[fallthrough]];
        }
        case kSearchSearching: {
            for (;;) {
                if (search.itemCount == search.pathStops.size()) {
                    auto& lastStop = search.pathStops.back();
                    u32 colTableIndex = m_collisionTable.getItem0(lastStop.nodeIndex) + 1;
                    search.pathStops.emplace_back(
                        colTableIndex - lastStop.nodeIndex - 1, colTableIndex, 0);
                }

                auto& pathStop = search.pathStops[search.itemCount];

                if (m_collisionTable.isItemPresent(pathStop.collisionPos++)) {
                    search.itemCount++;

                    if (isPathFragmentString(pathStop.nodeIndex)) {
                        u32 fragOffset = getPathFragmentOffset2(
                            pathStop.hiBitsIndex, pathStop.nodeIndex);

                        if (m_childDB) {
                            m_childDB->copyPathFragmentByIndex(search, fragOffset);
                        } else {
                            m_pathFragmentTable.copyFragment(search.pathBuffer, fragOffset);
                        }
                    } else {
                        search.pathBuffer.push_back(
                            static_cast<char>(m_loBitsTable[pathStop.nodeIndex]));
                    }

                    pathStop.savedPathLen = static_cast<u32>(search.pathBuffer.size());

                    if (m_fileNameIndexes.isItemPresent(pathStop.nodeIndex)) {
                        if (pathStop.fileNameIndex == kInvalidIndex) {
                            pathStop.fileNameIndex = m_fileNameIndexes.getItemValueAt(
                                pathStop.nodeIndex);
                        } else {
                            pathStop.fileNameIndex++;
                        }

                        search.foundPath = search.pathBuffer.data();
                        search.cchFoundPath = static_cast<u32>(search.pathBuffer.size());
                        search.nIndex = pathStop.fileNameIndex;
                        return true;
                    }
                } else {
                    if (search.itemCount == 1) {
                        search.searchPhase = kSearchFinished;
                        return false;
                    }

                    search.pathStops[search.itemCount - 1].nodeIndex++;
                    u32 prevCount = search.pathStops[search.itemCount - 2].savedPathLen;
                    search.pathBuffer.resize(prevCount);
                    search.itemCount--;
                }
            }
        }
        case kSearchFinished:
            break;
        }
        return false;
    }

    size_t fileNameCount() const { return m_fileNameIndexes.validItemCount(); }

private:
    bool loadFromStream(ByteStream& stream) {
        if (!m_collisionTable.loadFromStream(stream)) return false;
        if (!m_fileNameIndexes.loadFromStream(stream)) return false;
        if (!m_collisionHiBitsIndexes.loadFromStream(stream)) return false;

        if (!stream.getArray(m_loBitsTable, m_loBitsCount)) return false;
        if (!m_hiBitsTable.loadFromStream(stream)) return false;
        if (!m_pathFragmentTable.loadFromStream(stream)) return false;

        // If collision hi-bits are present but path fragments are empty,
        // there's a child database with the actual fragment data.
        if (m_collisionHiBitsIndexes.validItemCount() != 0 &&
            m_pathFragmentTable.empty()) {
            m_childDB = std::make_unique<FileNameDatabase>();
            if (!m_childDB->loadFromStream(stream)) return false;
        }

        if (!stream.getArray(m_hashTable, m_hashTableCount)) return false;
        m_hashTableMask = m_hashTableCount - 1;

        if (!stream.getU32(m_leafNodeBound)) return false;

        u32 bitMask;
        if (!stream.getU32(bitMask)) return false;
        // Struct10::sub_1957800 — just validate, we don't need the config values.

        return true;
    }

    bool isPathFragmentSingleChar(const HashEntry* entry) const {
        return (entry->fragmentOffset & kSingleCharMarker) == kSingleCharMarker;
    }

    bool isPathFragmentString(size_t index) const {
        return m_collisionHiBitsIndexes.isItemPresent(index);
    }

    u32 getPathFragmentOffset1(u32 indexLoBits) const {
        u32 indexHiBits = m_collisionHiBitsIndexes.getItemValueAt(indexLoBits);
        return (m_hiBitsTable.getItem(indexHiBits) << 8) | m_loBitsTable[indexLoBits];
    }

    u32 getPathFragmentOffset2(u32& indexHiBits, u32 indexLoBits) const {
        if (indexHiBits == kInvalidIndex) {
            indexHiBits = m_collisionHiBitsIndexes.getItemValueAt(indexLoBits);
        } else {
            indexHiBits++;
        }
        return (m_hiBitsTable.getItem(indexHiBits) << 8) | m_loBitsTable[indexLoBits];
    }

    bool comparePathFragment(SearchState& search) const {
        u32 nodeIdx = search.calcHashValue() & m_hashTableMask;
        auto entry = &m_hashTable[nodeIdx];

        if (entry->nodeIndex == search.nodeIndex) {
            if (!isPathFragmentSingleChar(entry)) {
                if (m_childDB) {
                    if (!m_childDB->comparePathFragmentByIndex(search, entry->childTableIndex))
                        return false;
                } else {
                    if (!m_pathFragmentTable.compareFragment(search.searchMask,
                            search.pathLength, entry->fragmentOffset))
                        return false;
                }
            } else {
                search.pathLength++;
            }
            search.nodeIndex = entry->nextIndex;
            return true;
        }

        // Collision resolution.
        u32 colTableIndex = m_collisionTable.getItem0(search.nodeIndex) + 1;
        search.nodeIndex = colTableIndex - search.nodeIndex - 1;
        u32 hiBitsIndex = kInvalidIndex;

        while (m_collisionTable.isItemPresent(colTableIndex)) {
            if (isPathFragmentString(search.nodeIndex)) {
                u32 fragOffset = getPathFragmentOffset2(hiBitsIndex, search.nodeIndex);
                u32 savePathLength = search.pathLength;

                if (m_childDB) {
                    if (m_childDB->comparePathFragmentByIndex(search, fragOffset))
                        return true;
                } else {
                    if (m_pathFragmentTable.compareFragment(search.searchMask,
                            search.pathLength, fragOffset))
                        return true;
                }
                if (search.pathLength != savePathLength) return false;
            } else {
                if (m_loBitsTable[search.nodeIndex] ==
                    static_cast<u8>(search.searchMask[search.pathLength])) {
                    search.pathLength++;
                    return true;
                }
            }
            search.nodeIndex++;
            colTableIndex++;
        }
        return false;
    }

    bool comparePathFragmentByIndex(SearchState& search, u32 tableIndex) const {
        for (;;) {
            auto entry = &m_hashTable[tableIndex & m_hashTableMask];
            if (tableIndex == entry->nextIndex) {
                if (!isPathFragmentSingleChar(entry)) {
                    if (m_childDB) {
                        if (!m_childDB->comparePathFragmentByIndex(search,
                                entry->childTableIndex))
                            return false;
                    } else {
                        if (!m_pathFragmentTable.compareFragment(search.searchMask,
                                search.pathLength, entry->fragmentOffset))
                            return false;
                    }
                } else {
                    if (search.searchMask[search.pathLength] != entry->singleChar)
                        return false;
                    search.pathLength++;
                }
                tableIndex = entry->nodeIndex;
                if (tableIndex == 0) return true;
                if (search.pathLength >= search.cchSearchMask) return false;
            } else {
                if (isPathFragmentString(tableIndex)) {
                    u32 fragOffset = getPathFragmentOffset1(tableIndex);
                    if (m_childDB) {
                        if (!m_childDB->comparePathFragmentByIndex(search, fragOffset))
                            return false;
                    } else {
                        if (!m_pathFragmentTable.compareFragment(search.searchMask,
                                search.pathLength, fragOffset))
                            return false;
                    }
                } else {
                    if (m_loBitsTable[tableIndex] !=
                        static_cast<u8>(search.searchMask[search.pathLength]))
                        return false;
                    search.pathLength++;
                }
                if (tableIndex <= m_leafNodeBound) return true;
                if (search.pathLength >= search.cchSearchMask) return false;
                tableIndex = m_collisionTable.getItem1(tableIndex) - tableIndex - 1;
            }
        }
    }

    bool compareAndCopyPathFragment(SearchState& search) const {
        u32 nodeIdx = search.calcHashValue() & m_hashTableMask;
        auto entry = &m_hashTable[nodeIdx];

        if (search.nodeIndex == entry->nodeIndex) {
            if (!isPathFragmentSingleChar(entry)) {
                if (m_childDB) {
                    if (!m_childDB->compareAndCopyPathFragmentByIndex(search,
                            entry->childTableIndex))
                        return false;
                } else {
                    if (!m_pathFragmentTable.compareAndCopyFragment(
                            search.searchMask, search.cchSearchMask,
                            search.pathBuffer, search.pathLength,
                            entry->fragmentOffset))
                        return false;
                }
            } else {
                search.pathBuffer.push_back(entry->singleChar);
                search.pathLength++;
            }
            search.nodeIndex = entry->nextIndex;
            return true;
        }

        // Collision resolution.
        u32 colTableIndex = m_collisionTable.getItem0(search.nodeIndex) + 1;
        search.nodeIndex = colTableIndex - search.nodeIndex - 1;
        u32 hiBitsIndex = kInvalidIndex;

        while (m_collisionTable.isItemPresent(colTableIndex)) {
            if (isPathFragmentString(search.nodeIndex)) {
                u32 fragOffset = getPathFragmentOffset2(hiBitsIndex, search.nodeIndex);
                u32 savePathLength = search.pathLength;

                if (m_childDB) {
                    if (m_childDB->compareAndCopyPathFragmentByIndex(search, fragOffset))
                        return true;
                } else {
                    if (m_pathFragmentTable.compareAndCopyFragment(
                            search.searchMask, search.cchSearchMask,
                            search.pathBuffer, search.pathLength, fragOffset))
                        return true;
                }
                if (savePathLength != search.pathLength) return false;
            } else {
                if (m_loBitsTable[search.nodeIndex] ==
                    static_cast<u8>(search.searchMask[search.pathLength])) {
                    search.pathBuffer.push_back(
                        static_cast<char>(m_loBitsTable[search.nodeIndex]));
                    search.pathLength++;
                    return true;
                }
            }
            search.nodeIndex++;
            colTableIndex++;
        }
        return false;
    }

    bool compareAndCopyPathFragmentByIndex(SearchState& search, u32 tableIndex) const {
        for (;;) {
            auto entry = &m_hashTable[tableIndex & m_hashTableMask];
            if (tableIndex == entry->nextIndex) {
                if (!isPathFragmentSingleChar(entry)) {
                    if (m_childDB) {
                        if (!m_childDB->compareAndCopyPathFragmentByIndex(search,
                                entry->childTableIndex))
                            return false;
                    } else {
                        if (!m_pathFragmentTable.compareAndCopyFragment(
                                search.searchMask, search.cchSearchMask,
                                search.pathBuffer, search.pathLength,
                                entry->fragmentOffset))
                            return false;
                    }
                } else {
                    if (entry->singleChar != search.searchMask[search.pathLength])
                        return false;
                    search.pathBuffer.push_back(entry->singleChar);
                    search.pathLength++;
                }
                tableIndex = entry->nodeIndex;
                if (tableIndex == 0) return true;
            } else {
                if (isPathFragmentString(tableIndex)) {
                    u32 fragOffset = getPathFragmentOffset1(tableIndex);
                    if (m_childDB) {
                        if (!m_childDB->compareAndCopyPathFragmentByIndex(search, fragOffset))
                            return false;
                    } else {
                        if (!m_pathFragmentTable.compareAndCopyFragment(
                                search.searchMask, search.cchSearchMask,
                                search.pathBuffer, search.pathLength, fragOffset))
                            return false;
                    }
                } else {
                    if (m_loBitsTable[tableIndex] !=
                        static_cast<u8>(search.searchMask[search.pathLength]))
                        return false;
                    search.pathBuffer.push_back(
                        static_cast<char>(m_loBitsTable[tableIndex]));
                    search.pathLength++;
                }
                if (tableIndex <= m_leafNodeBound) return true;
                if (search.pathLength >= search.cchSearchMask) break;
                tableIndex = ~tableIndex + m_collisionTable.getItem1(tableIndex);
            }
            if (search.pathLength >= search.cchSearchMask) break;
        }

        copyPathFragmentByIndex(search, tableIndex);
        return true;
    }

    void copyPathFragmentByIndex(SearchState& search, u32 tableIndex) const {
        for (;;) {
            auto entry = &m_hashTable[tableIndex & m_hashTableMask];
            if (tableIndex == entry->nextIndex) {
                if (!isPathFragmentSingleChar(entry)) {
                    if (m_childDB) {
                        m_childDB->copyPathFragmentByIndex(search, entry->childTableIndex);
                    } else {
                        m_pathFragmentTable.copyFragment(search.pathBuffer,
                                                         entry->fragmentOffset);
                    }
                } else {
                    search.pathBuffer.push_back(entry->singleChar);
                }
                tableIndex = entry->nodeIndex;
                if (tableIndex == 0) return;
            } else {
                if (isPathFragmentString(tableIndex)) {
                    u32 fragOffset = getPathFragmentOffset1(tableIndex);
                    if (m_childDB) {
                        m_childDB->copyPathFragmentByIndex(search, fragOffset);
                    } else {
                        m_pathFragmentTable.copyFragment(search.pathBuffer, fragOffset);
                    }
                } else {
                    search.pathBuffer.push_back(
                        static_cast<char>(m_loBitsTable[tableIndex]));
                }
                if (tableIndex <= m_leafNodeBound) return;
                tableIndex = ~tableIndex + m_collisionTable.getItem1(tableIndex);
            }
        }
    }

    SparseArray m_collisionTable;
    SparseArray m_fileNameIndexes;
    SparseArray m_collisionHiBitsIndexes;
    const u8* m_loBitsTable = nullptr;
    u32 m_loBitsCount = 0;
    BitEntryArray m_hiBitsTable;
    PathFragmentTable m_pathFragmentTable;
    std::unique_ptr<FileNameDatabase> m_childDB;
    const HashEntry* m_hashTable = nullptr;
    u32 m_hashTableCount = 0;
    u32 m_hashTableMask = 0;
    u32 m_leafNodeBound = 0;
};

// ============================================================================
// MarFile — wrapper for a MAR database within the root data
// ============================================================================

class MarFile {
public:
    bool load(const u8* marData, size_t marDataSize) {
        // We need to keep a copy because FileNameDatabase references into the data.
        m_data.assign(marData, marData + marDataSize);
        return m_database.load(m_data.data(), m_data.size());
    }

    bool searchFile(SearchState& search) const {
        return m_database.findFile(search);
    }

    bool doSearch(SearchState& search, bool& found) const {
        found = m_database.doSearch(search);
        return true;
    }

    size_t fileNameCount() const { return m_database.fileNameCount(); }

private:
    std::vector<u8> m_data;
    FileNameDatabase m_database;
};

// ============================================================================
// MNDX file structures
// ============================================================================

struct MndxHeader {
    u32 signature;
    u32 headerVersion;
    u32 formatVersion;
};

struct MarInfo {
    u32 marIndex;
    u32 marDataSize;
    u32 marDataSizeHi;
    u32 marDataOffset;
    u32 marDataOffsetHi;
};

static_assert(sizeof(MarInfo) == 20, "MarInfo must be 20 bytes");

struct MndxCKeyEntry {
    u32 flags;
    u8 cKey[16];
    u32 contentSize;
};

static_assert(sizeof(MndxCKeyEntry) == 24, "MndxCKeyEntry must be 24 bytes");

// ============================================================================
// MndxRoot implementation
// ============================================================================

std::unique_ptr<MndxRoot> MndxRoot::parse(std::span<const u8> data,
                                           interfaces::WorkerPool* pool) {
    ensureBitPosTable();

    if (data.size() < sizeof(MndxHeader))
        return nullptr;

    const u8* ptr = data.data();
    const u8* end = ptr + data.size();

    // Read and validate header.
    MndxHeader header;
    std::memcpy(&header, ptr, sizeof(header));

    if (header.signature != kMndxSignature)
        return nullptr;
    if (header.formatVersion < 1 || header.formatVersion > 2)
        return nullptr;
    if (header.headerVersion > 2)
        return nullptr;

    size_t offset = sizeof(MndxHeader);

    // Header version 2 has 2 extra DWORDs.
    if (header.headerVersion == 2) {
        if (offset + 8 > data.size()) return nullptr;
        offset += 8; // Skip field_1C and field_20.
    }

    // Read the MNDX info fields.
    if (offset + 0x1C > data.size()) return nullptr;

    u32 marInfoOffset  = readLE32(ptr + offset + 0x00);
    u32 marInfoCount   = readLE32(ptr + offset + 0x04);
    u32 marInfoSize    = readLE32(ptr + offset + 0x08);
    u32 ckeyOffset     = readLE32(ptr + offset + 0x0C);
    u32 ckeyCount      = readLE32(ptr + offset + 0x10);
    u32 fileNameCount  = readLE32(ptr + offset + 0x14);
    u32 ckeyEntrySize  = readLE32(ptr + offset + 0x18);

    if (marInfoCount > kMarCount) return nullptr;
    if (marInfoSize != sizeof(MarInfo)) return nullptr;
    if (ckeyEntrySize != sizeof(MndxCKeyEntry)) return nullptr;

    // Load MAR files.
    std::unique_ptr<MarFile> marFiles[kMarCount];

    for (u32 i = 0; i < marInfoCount; ++i) {
        size_t marInfoPos = marInfoOffset + static_cast<size_t>(marInfoSize) * i;
        if (marInfoPos + sizeof(MarInfo) > data.size()) return nullptr;

        MarInfo mi;
        std::memcpy(&mi, ptr + marInfoPos, sizeof(mi));

        size_t marDataOff = mi.marDataOffset;
        size_t marDataSz  = mi.marDataSize;

        if (marDataOff + marDataSz > data.size()) return nullptr;

        marFiles[i] = std::make_unique<MarFile>();
        if (!marFiles[i]->load(ptr + marDataOff, marDataSz))
            return nullptr;
    }

    // Validate all 3 MAR files loaded.
    if (!marFiles[kMarPackageNames] || !marFiles[kMarStrippedNames] ||
        !marFiles[kMarFullNames])
        return nullptr;

    // Validate file name count.
    if (marFiles[kMarStrippedNames]->fileNameCount() != fileNameCount)
        return nullptr;

    // Load CKey entries.
    size_t ckeyDataSize = static_cast<size_t>(ckeyCount) * ckeyEntrySize;
    if (ckeyOffset + ckeyDataSize > data.size()) return nullptr;

    auto* ckeyEntries = reinterpret_cast<const MndxCKeyEntry*>(ptr + ckeyOffset);

    // Build FileNameIndex → first CKey entry mapping.
    std::vector<const MndxCKeyEntry*> fileNameToCKey(fileNameCount + 1);
    {
        u32 nameIndex = 0;
        fileNameToCKey[nameIndex++] = ckeyEntries;
        for (u32 i = 0; i < ckeyCount; ++i) {
            if (nameIndex > fileNameCount) break;
            if (ckeyEntries[i].flags & kMndxLastCKeyEntry) {
                fileNameToCKey[nameIndex++] = &ckeyEntries[i] + 1;
            }
        }
        if (nameIndex - 1 != fileNameCount)
            return nullptr;
    }

    // Load package names from MAR[0].
    std::vector<std::string> packages;
    {
        SearchState search;
        search.searchMask = "";
        search.cchSearchMask = 0;
        bool found = false;

        size_t expectedPackages = marFiles[kMarPackageNames]->fileNameCount();
        packages.resize(expectedPackages);

        while (marFiles[kMarPackageNames]->doSearch(search, found) && found) {
            if (search.nIndex < expectedPackages) {
                packages[search.nIndex] = std::string(search.foundPath, search.cchFoundPath);
            }
        }
    }

    // Enumerate all file names from MAR[1] (stripped names) and build entries.
    auto root = std::make_unique<MndxRoot>();
    {
        SearchState search;
        search.searchMask = "";
        search.cchSearchMask = 0;
        bool found = false;

        while (marFiles[kMarStrippedNames]->doSearch(search, found) && found) {
            if (search.nIndex >= fileNameCount) continue;

            std::string strippedName(search.foundPath, search.cchFoundPath);

            // Get all CKey entries for this file name (across packages).
            const MndxCKeyEntry* entryPtr = fileNameToCKey[search.nIndex];
            const MndxCKeyEntry* ckeyEnd = ckeyEntries + ckeyCount;

            while (entryPtr < ckeyEnd) {
                u32 packageIndex = entryPtr->flags & kPackageIndexMask;

                // Build full path: packageName/strippedName.
                std::string fullPath;
                if (packageIndex < packages.size() && !packages[packageIndex].empty()) {
                    fullPath = packages[packageIndex] + "/" + strippedName;
                } else {
                    fullPath = strippedName;
                }

                RootEntry entry;
                std::memcpy(entry.cKey.data(), entryPtr->cKey, kCKeySize);
                entry.path = std::move(fullPath);

                root->m_entries.push_back(std::move(entry));

                bool isLast = (entryPtr->flags & kMndxLastCKeyEntry) != 0;
                entryPtr++;
                if (isLast) break;
            }
        }
    }

    root->buildIndices();
    return root;
}

std::vector<const RootEntry*> MndxRoot::findByPath(const std::string& path) const {
    auto key = normalizeCascPath(path);
    return findByNormalizedPath(key);
}

std::vector<const RootEntry*> MndxRoot::findByNormalizedPath(const std::string& normalizedPath) const {
    return m_byPath.findAll(m_entries, normalizedPath);
}

bool MndxRoot::hasPath(const std::string& normalizedPath) const {
    return m_byPath.contains(normalizedPath);
}

std::vector<const RootEntry*> MndxRoot::findByFileDataId(u32 /*fileDataId*/, FileIdHint /*hint*/) const {
    // MNDX roots don't use FileDataId.
    return {};
}

void MndxRoot::buildIndices() {
    m_byPath.clear();
    m_byPath.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (!m_entries[i].path.empty())
            m_byPath.emplace(normalizeCascPath(m_entries[i].path), i);
    }
}

} // namespace whiteout::storages::casc
