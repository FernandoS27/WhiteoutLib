// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../common/byte_order.h"
#include "../../common/string_utils.h"
#include "../storage/constants.h"
#include "common/root_build_utils.h"
#include "common/wow_tvfs_path.h"
#include "tvfs_root.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string_view>

namespace whiteout::storages::casc {

using storages::common::normalizeCascPath;
using storages::common::readBE16;
using storages::common::readBE32;
using storages::common::readBEVar;
using storages::common::readLE32;

// ---- Constants local to TVFS root parser ----

/// TVFS minimum header size.
static constexpr size_t kTvfsMinHeaderSize = 42;

/// TVFS flag: include CKey in content-file table entries.
static constexpr u32 kTvfsFlagIncludeCKey = 0x0001;

/// Maximum recursion depth for TVFS path tree traversal.
static constexpr int kTvfsMaxTraversalDepth = 128;

/// End-of-chain sentinel for m_chainNext.
static constexpr u32 kNoChain = UINT32_MAX;

/// TVFS EKey size. CascLib enforces this; parseHeader rejects anything else.
static constexpr u32 kTvfsEKeySize = 9;

/// Hash an already-normalized path for use as a FlatHashMap key.
static u64 pathHash64(const std::string& s) {
    return storages::common::cascPathHash64(s);
}

namespace {

// ============================================================================
// TVFS header
// ============================================================================

/// TVFS flags.
// constexpr u32 kTvfsFlagWriteSupport = 0x0002;
// constexpr u32 kTvfsFlagPatchSupport = 0x0004;
// constexpr u32 kTvfsFlagLowescaseManifest = 0x0008;

struct TvfsHeader {
    u32 signature = 0;
    u8 formatVersion = 0;
    u8 headerSize = 0;
    u8 eKeySize = 0;
    u8 patchKeySize = 0;
    u32 flags = 0;           // LE!
    u32 pathTableOffset = 0; // BE
    u32 pathTableSize = 0;   // BE
    u32 vfsTableOffset = 0;  // BE
    u32 vfsTableSize = 0;    // BE
    u32 cftTableOffset = 0;  // BE
    u32 cftTableSize = 0;    // BE
    u16 maxDepth = 0;        // BE
    u32 estTableOffset = 0;  // BE
    u32 estTableSize = 0;    // BE
};

/// Minimum header size: sig(4)+ver(1)+hdrSz(1)+eKey(1)+pKey(1)
///   +flags(4)+6*off/sz(24)+maxDepth(2)+2*est(8) = 46 bytes.
// kTvfsMinHeaderSize is defined in constants.h.

static bool parseHeader(std::span<const u8> data, TvfsHeader& hdr) {
    if (data.size() < kTvfsMinHeaderSize)
        return false;

    const u8* p = data.data();
    hdr.signature = readLE32(p);
    if (hdr.signature != RootSignature::kTVFS)
        return false;

    hdr.formatVersion = p[4];
    if (hdr.formatVersion != kTvfsFormatVersion)
        return false;

    hdr.headerSize = p[5];
    hdr.eKeySize = p[6];
    hdr.patchKeySize = p[7];

    // CascLib enforces eKeySize == 9 (ERROR_BAD_FORMAT otherwise).
    // All known TVFS manifests use 9-byte eKeys matching CDN index key size.
    if (hdr.eKeySize != kTvfsEKeySize)
        return false;

    hdr.flags = readLE32(p + 8);
    hdr.pathTableOffset = readBE32(p + 12);
    hdr.pathTableSize = readBE32(p + 16);
    hdr.vfsTableOffset = readBE32(p + 20);
    hdr.vfsTableSize = readBE32(p + 24);
    hdr.cftTableOffset = readBE32(p + 28);
    hdr.cftTableSize = readBE32(p + 32);
    hdr.maxDepth = readBE16(p + 36);
    hdr.estTableOffset = readBE32(p + 38);
    hdr.estTableSize = readBE32(p + 42);

    // Sanity check offsets within data.
    if (hdr.pathTableOffset + hdr.pathTableSize > data.size())
        return false;
    if (hdr.vfsTableOffset + hdr.vfsTableSize > data.size())
        return false;
    if (hdr.cftTableOffset + hdr.cftTableSize > data.size())
        return false;

    return true;
}

/// Determine byte-size of a CFT offset field based on CFT table size.
static u32 getCftOffsSize(u32 cftTableSize) {
    if (cftTableSize <= 0xFF)
        return 1;
    if (cftTableSize <= 0xFFFF)
        return 2;
    if (cftTableSize <= 0xFFFFFF)
        return 3;
    return 4;
}

// ============================================================================
// Path table traversal
// ============================================================================

/// A deferred sub-manifest traversal — queued during the root-blob pass so the
/// independent sub-manifests can be traversed in parallel.
struct SubManifestJob {
    std::array<u8, 16> eKey{}; ///< Zero-padded; first kTvfsEKeySize bytes valid.
    std::string containerPath; ///< Path prefix ("parent:") for this sub-manifest.
};

/// A deferred subtree traversal — a folder node whose recursion was deferred so
/// it can be traversed in parallel. Carries its blob context so subtrees from
/// the root blob and from (giant) sub-manifests can share one work queue.
/// data/node/nodeEnd point into a blob that outlives the parallel section.
struct SubtreeJob {
    std::span<const u8> data;
    TvfsHeader hdr;
    u32 cftOffsSize = 0;
    const u8* node = nullptr;
    const u8* nodeEnd = nullptr;
    std::string accumulated; ///< Path accumulated down to this folder.
    int depth = 0;
};

/// The VFS sub-manifest EKeys, packed for membership testing.
///
/// TVFS pins eKeySize at 9 (parseHeader rejects anything else), so each key
/// fits in a u64 plus one trailing byte and every binary-search step becomes an
/// integer compare instead of a memcmp call.
struct VfsKeySet {
    std::vector<std::pair<u64, u8>> keys; ///< Sorted.

    /// One bit per 16-bit key prefix. A manifest names far more files than it
    /// has sub-manifests (3.2M against 864 on WoW retail), so nearly every
    /// query is a miss and this rejects it from an 8 KB table instead of
    /// walking the search.
    std::array<u64, 1024> prefixBits{};

    static std::pair<u64, u8> pack(const u8* k) {
        u64 hi = 0;
        for (int i = 0; i < 8; ++i)
            hi = (hi << 8) | k[i];
        return {hi, k[8]};
    }

    static u32 prefixOf(const u8* k) {
        return (u32(k[0]) << 8) | k[1];
    }

    void build(const std::vector<std::array<u8, 16>>& src) {
        keys.clear();
        keys.reserve(src.size());
        prefixBits.fill(0);
        for (auto& k : src) {
            keys.push_back(pack(k.data()));
            u32 const p = prefixOf(k.data());
            prefixBits[p >> 6] |= u64(1) << (p & 63);
        }
        std::sort(keys.begin(), keys.end());
    }

    bool empty() const {
        return keys.empty();
    }

    bool contains(const u8* eKey) const {
        u32 const p = prefixOf(eKey);
        if ((prefixBits[p >> 6] & (u64(1) << (p & 63))) == 0)
            return false;
        auto const probe = pack(eKey);
        auto it = std::lower_bound(keys.begin(), keys.end(), probe);
        return it != keys.end() && *it == probe;
    }
};

/// Context for recursive path-tree traversal.
struct TraversalCtx {
    std::span<const u8> data;
    const TvfsHeader& hdr;
    u32 cftOffsSize;
    std::vector<RootEntry>& entries;
    const VfsResolver* resolver; ///< null when no sub-container resolution.
    const VfsKeySet* vfsKeys;    ///< null when no sub-container resolution.
    /// When non-null, sub-containers are queued here instead of recursed into
    /// inline — lets the caller traverse them in parallel. Null in job contexts
    /// (nested sub-containers recurse inline within their parent job).
    std::vector<SubManifestJob>* pendingJobs = nullptr;
    /// When non-null, folder nodes are queued here instead of recursed into —
    /// turns traversePathTree into a single-level walk so the caller can
    /// descend the tree breadth-first and farm out subtrees in parallel.
    std::vector<SubtreeJob>* pendingSubtrees = nullptr;
    TvfsLeafDecode leafDecode = TvfsLeafDecode::None;
};

static void traversePathTree(TraversalCtx& ctx, const u8* node, const u8* nodeEnd,
                             const std::string& accumulated, int depth);

/// Forward-declare parsePathTable so sub-container resolution can call it recursively.
static void parsePathTable(TraversalCtx& ctx, const std::string& pathPrefix = {});

/// Copy up to 16 bytes from `src` (size `srcSize`) into a zero-padded 16-byte key.
static void copyKey16(std::array<u8, 16>& dst, const u8* src, u32 srcSize) {
    std::memset(dst.data(), 0, 16);
    std::memcpy(dst.data(), src, std::min<u32>(srcSize, 16));
}

/// Membership test against the VFS sub-manifest EKeys. Every single-span entry
/// asks it, so on WoW retail that is 3.2M queries against ~864 keys, and a
/// 9-byte memcmp per binary-search step cost more than the traversal around it.
/// Packing each key into a (first 8 bytes, 9th byte) pair makes every step an
/// integer compare over an array that stays in L1.
static bool isVfsSubManifest(const TraversalCtx& ctx, const u8* eKey) {
    if (!ctx.vfsKeys || ctx.vfsKeys->empty())
        return false;
    return ctx.vfsKeys->contains(eKey);
}

/// If a single-span entry resolves to a known VFS sub-manifest, emit a
/// container entry, recurse into the sub-manifest with a `path:` prefix,
/// and return true. Returns false to fall through to the normal-entry path.
static bool tryEmitSubContainer(TraversalCtx& ctx, std::string_view path, size_t vfsPos,
                                u32 spanEntrySize, bool hasCKey) {
    if (!ctx.resolver || !ctx.vfsKeys)
        return false;

    auto& hdr = ctx.hdr;
    const u8* vfsBase = ctx.data.data() + hdr.vfsTableOffset;
    const u8* cftBase = ctx.data.data() + hdr.cftTableOffset;

    if (vfsPos + spanEntrySize > hdr.vfsTableSize)
        return false;

    u32 const cftOffset = readBEVar(vfsBase + vfsPos + 8, ctx.cftOffsSize);
    u32 const cftEntrySize = hasCKey ? (hdr.eKeySize * 2) : hdr.eKeySize;
    if (cftOffset + cftEntrySize > hdr.cftTableSize)
        return false;

    const u8* eKeyPtr = cftBase + cftOffset;
    if (!isVfsSubManifest(ctx, eKeyPtr))
        return false;

    // CascLib uses ':' to separate the sub-container from its parent path.
    std::string containerPath(path);
    containerPath += ':';

    RootEntry containerEntry;
    containerEntry.path = containerPath;
    copyKey16(containerEntry.eKey, eKeyPtr, hdr.eKeySize);
    if (hasCKey)
        copyKey16(containerEntry.cKey, eKeyPtr + hdr.eKeySize, hdr.eKeySize);
    ctx.entries.push_back(std::move(containerEntry));

    // Defer to the caller for parallel traversal when a job queue is set.
    if (ctx.pendingJobs) {
        SubManifestJob job;
        copyKey16(job.eKey, eKeyPtr, hdr.eKeySize);
        job.containerPath = std::move(containerPath);
        ctx.pendingJobs->push_back(std::move(job));
        return true;
    }

    // Inline path: resolve + recurse (job contexts, or no worker pool).
    auto subData = (*ctx.resolver)(std::span<const u8>(eKeyPtr, hdr.eKeySize));
    if (!subData.empty()) {
        TvfsHeader subHdr;
        if (parseHeader(subData, subHdr)) {
            u32 const subCftOffsSize = getCftOffsSize(subHdr.cftTableSize);
            TraversalCtx subCtx{subData,     subHdr,  subCftOffsSize, ctx.entries,   ctx.resolver,
                                ctx.vfsKeys, nullptr, nullptr,        ctx.leafDecode};
            parsePathTable(subCtx, containerPath);
        }
    }
    return true;
}

/// Leaf node: resolve VFS → CFT → EKey [+ CKey], emit one entry per span.
static void processFileEntry(TraversalCtx& ctx, std::string_view path, u32 vfsOffset) {
    auto& hdr = ctx.hdr;
    const u8* vfsBase = ctx.data.data() + hdr.vfsTableOffset;
    const u8* cftBase = ctx.data.data() + hdr.cftTableOffset;

    if (vfsOffset >= hdr.vfsTableSize)
        return;

    // VFS entry: SpanCount(u8), then per span: FileOffset(u32 BE) + SpanSize(u32 BE) +
    // CftOffset(var BE).
    u8 const spanCount = vfsBase[vfsOffset];
    if (spanCount == 0 || spanCount > 224)
        return;

    size_t vfsPos = vfsOffset + 1;
    u32 const spanEntrySize = 4 + 4 + ctx.cftOffsSize;
    bool const hasCKey = (hdr.flags & kTvfsFlagIncludeCKey) != 0;

    if (spanCount == 1 && tryEmitSubContainer(ctx, path, vfsPos, spanEntrySize, hasCKey))
        return;

    // Decoded once for the whole leaf: every span shares the name, and the name
    // is in cache here. A decorator doing this afterwards walks several million
    // scattered strings instead.
    wow_tvfs_path::Info wowInfo;
    bool wowDecoded = false;
    if (ctx.leafDecode != TvfsLeafDecode::None)
        wowDecoded = wow_tvfs_path::tryDecode(path, wowInfo);
    bool const keepPath = !wowDecoded || ctx.leafDecode != TvfsLeafDecode::WowDropPath;

    for (u8 s = 0; s < spanCount; ++s) {
        if (vfsPos + spanEntrySize > hdr.vfsTableSize)
            return;

        u32 const cftOffset = readBEVar(vfsBase + vfsPos + 8, ctx.cftOffsSize);
        vfsPos += spanEntrySize;

        // CFT entry: EKey(eKeySize) [+ CKey(eKeySize) if IncludeCKey].
        u32 const cftEntrySize = hasCKey ? (hdr.eKeySize * 2) : hdr.eKeySize;
        if (cftOffset + cftEntrySize > hdr.cftTableSize)
            continue;

        // Built in place: a local RootEntry would be zero-filled, then moved
        // 128 bytes into the vector, for each of 3.2M entries. The keys need no
        // clearing either — emplace_back already zeroed them.
        auto& entry = ctx.entries.emplace_back();
        if (keepPath)
            entry.path = path;
        std::memcpy(entry.eKey.data(), cftBase + cftOffset, hdr.eKeySize);
        if (hasCKey)
            std::memcpy(entry.cKey.data(), cftBase + cftOffset + hdr.eKeySize, hdr.eKeySize);
        // else: cKey stays zero — resolution must use eKey directly.

        // After the CFT copy, which only carries a truncated CKey where the
        // encoded name has the full 16 bytes.
        if (wowDecoded) {
            entry.cKey = wowInfo.cKey;
            entry.localeFlags = wowInfo.localeFlags;
            entry.contentFlags = wowInfo.contentFlags;
            entry.fileDataId = wowInfo.fileDataId;
        }
    }
}

/// Path bytes as CASC stores them: forward slashes become backslashes and
/// ASCII upper case becomes lower case. Locale-free by construction —
/// std::tolower would consult the C locale for every one of the ~170M
/// characters a WoW retail traversal normalizes.
inline constexpr std::array<u8, 256> kNormalizedPathChar = [] {
    std::array<u8, 256> t{};
    for (size_t i = 0; i < 256; ++i)
        t[i] = static_cast<u8>(i);
    for (u8 c = 'A'; c <= 'Z'; ++c)
        t[c] = static_cast<u8>(c + ('a' - 'A'));
    t[static_cast<size_t>('/')] = static_cast<u8>('\\');
    return t;
}();

/// Recursive prefix-tree traversal.
///
/// Matches CascLib's ParsePathFileTable / CapturePathEntry / PathBuffer_AppendNode
/// semantics exactly:
///
/// Each path entry is: [pre-0x00?] [len][name] [post-0x00?] [0xFF nodeValue?]
///
/// - Pre-0x00 inserts '/' before the name fragment.
/// - Post-0x00 inserts '/' after the name fragment.
/// - After the name, if the next byte is NOT 0x00 and NOT 0xFF (i.e. another
///   length byte), CascLib treats this as an implicit post-separator.
/// - Entries WITHOUT a node value (0xFF) are "prefix fragments" — they accumulate
///   in the path buffer. The buffer is only restored after processing an entry
///   that DOES have a node value.
/// - 0xFF + folder nodeValue (bit 31 set): recurse into sub-directory contents.
/// - 0xFF + file nodeValue (bit 31 clear): leaf file → emit entry.
///
/// @param accumulated  Path built so far (prefix fragments + parent path).
static void traversePathTree(TraversalCtx& ctx, const u8* node, const u8* nodeEnd,
                             const std::string& accumulated, int depth) {
    if (depth > kTvfsMaxTraversalDepth)
        return; // safety limit

    std::string pathBuf = accumulated;

    while (node < nodeEnd) {
        // --- CapturePathEntry equivalent ---

        // Pre-0x00: path separator before name.
        if (*node == kTvfsPathSeparator) {
            pathBuf += '\\';
            ++node;
            if (node >= nodeEnd)
                break;
        }

        // Name fragment: [len][bytes]. Appended (and normalized) below rather
        // than materialized here — a temporary would be a heap allocation per
        // node, and WoW retail walks 3.2M of them.
        const u8* nameData = nullptr;
        u8 nameLen = 0;
        if (node < nodeEnd && *node != kTvfsNodeValueMarker) {
            nameLen = *node++;
            if (node + nameLen > nodeEnd)
                return;
            nameData = node;
            node += nameLen;
        }

        // Post-0x00: path separator after name.
        bool hasPostSep = false;
        if (node < nodeEnd && *node == kTvfsPathSeparator) {
            hasPostSep = true;
            ++node;
        }

        bool hasNodeValue = false;
        u32 nodeValue = 0;
        if (node < nodeEnd) {
            if (*node == kTvfsNodeValueMarker) {
                // 0xFF marker: node value follows.
                ++node;
                if (node + 4 > nodeEnd)
                    break;
                nodeValue = readBE32(node);
                node += 4;
                hasNodeValue = true;
            } else if (!hasPostSep) {
                // Next byte is another name-length → implicit post-separator.
                hasPostSep = true;
            }
        }

        // --- PathBuffer_AppendNode equivalent ---
        // (pre-separator was already appended above)
        //
        // Normalized as it is copied so the path is ready for lookup without a
        // post-pass over all entries. Through a table and a raw pointer: this
        // runs over ~170M characters on WoW retail, and going through
        // std::string::operator[] repeats the small-string check per character.
        if (nameLen > 0) {
            size_t const nameAt = pathBuf.size();
            pathBuf.resize(nameAt + nameLen);
            char* out = pathBuf.data() + nameAt;
            for (u8 i = 0; i < nameLen; ++i)
                out[i] = static_cast<char>(kNormalizedPathChar[nameData[i]]);
        }
        if (hasPostSep)
            pathBuf += '\\';

        // --- Process node value ---
        if (hasNodeValue) {
            if (nodeValue & kTvfsFolderNodeBit) {
                // Folder node: recurse into children.
                u32 const folderDataLen = nodeValue & ~kTvfsFolderNodeBit;
                if (folderDataLen < 4)
                    break;
                u32 const innerLen = folderDataLen - 4;
                if (node + innerLen > nodeEnd)
                    break;

                if (ctx.pendingSubtrees) {
                    // Single-level mode: defer this folder's subtree.
                    ctx.pendingSubtrees->push_back(SubtreeJob{ctx.data, ctx.hdr, ctx.cftOffsSize,
                                                              node, node + innerLen, pathBuf,
                                                              depth + 1});
                } else {
                    traversePathTree(ctx, node, node + innerLen, pathBuf, depth + 1);
                }
                node += innerLen;
            } else {
                // File node — strip leading/trailing separators that the TVFS
                // format's pre-0x00 (sibling boundary) and post-0x00 (before
                // 0xFF) can leave. normalizeCascPath would do this, but we
                // skip it in preNormalized mode.
                size_t s = 0;
                while (s < pathBuf.size() && pathBuf[s] == '\\')
                    ++s;
                size_t e = pathBuf.size();
                while (e > s && pathBuf[e - 1] == '\\')
                    --e;
                processFileEntry(ctx, std::string_view(pathBuf).substr(s, e - s), nodeValue);
            }

            // Restore path buffer to the save-point (undo this entry + any
            // accumulated prefix fragments since function entry).
            pathBuf = accumulated;
        }
        // No node value → prefix fragment stays in pathBuf for the next entry.
    }
}

/// Entry point: unwrap the optional anonymous root folder, then traverse.
/// CascLib does this unwrap in ParseDirectoryData before calling ParsePathFileTable.
static void parsePathTable(TraversalCtx& ctx, const std::string& pathPrefix) {
    const u8* node = ctx.data.data() + ctx.hdr.pathTableOffset;
    const u8* nodeEnd = node + ctx.hdr.pathTableSize;

    // Most TVFS blobs start with an anonymous root folder: 0xFF + folder nodeValue.
    if (node < nodeEnd && *node == kTvfsNodeValueMarker) {
        if (node + 5 > nodeEnd)
            return;
        u32 const nodeValue = readBE32(node + 1);
        if (nodeValue & kTvfsFolderNodeBit) {
            u32 const folderDataLen = nodeValue & ~kTvfsFolderNodeBit;
            if (folderDataLen < 4)
                return;
            u32 const innerLen = folderDataLen - 4;
            const u8* childStart = node + 5; // past 0xFF + 4-byte value
            const u8* childEnd = childStart + innerLen;
            if (childEnd > nodeEnd)
                return;
            traversePathTree(ctx, childStart, childEnd, pathPrefix, 0);
            return;
        }
    }

    // No root wrapper — parse directly.
    traversePathTree(ctx, node, nodeEnd, pathPrefix, 0);
}

} // anonymous namespace

// Path trie helpers are now in common/path_trie.h.

// ============================================================================
// TvfsRoot public API
// ============================================================================

namespace {

/// Min entry count for a sub-manifest to be "big" — big ones are descended and
/// fanned out alongside the root blob; small ones become whole-blob jobs.
static constexpr u32 kBigBlobMinEntries = 4000;

/// A folder subtree larger than this many path-table bytes is split off into
/// its own parallel job. The byte length of a folder node's inner data is a
/// good proxy for its entry count, so this caps the size of any single work
/// unit and keeps the pool balanced even when the directory tree is very lumpy
/// (e.g. WoW's giant `world/` subtree). The driver thread does the splitting —
/// jobs never spawn jobs, which would risk a worker-pool deadlock.
static constexpr size_t kSubtreeSplitBytes = 48 * 1024;

/// Safety cap on the driver's folder-splitting descent depth.
static constexpr int kMaxSplitDepth = 32;

/// Shared sink for the parallel traversal: each job appends its result as a
/// private buffer (concatenated once at the end) under a short-held lock.
struct ParallelTraverseState {
    const VfsResolver* resolver = nullptr;
    const VfsKeySet* vfsKeys = nullptr;
    TvfsLeafDecode leafDecode = TvfsLeafDecode::None;
    /// One slot per job, each written by exactly one worker and concatenated in
    /// job order. Entry order decides which variant a FileDataId with several
    /// locale/content candidates resolves to, so it has to be reproducible —
    /// collecting buffers as jobs finished made that depend on thread timing.
    std::vector<std::vector<RootEntry>> buffers;
};

/// Traverse one (already small-enough) folder subtree fully into its slot.
/// Entries accumulate in a local vector first — the slots are adjacent in one
/// array, so growing them in place would false-share their control blocks.
static void traverseSubtreeJob(ParallelTraverseState& st, const SubtreeJob& job, size_t slot) {
    std::vector<RootEntry> local;
    TraversalCtx ctx{job.data,   job.hdr, job.cftOffsSize, local,        st.resolver,
                     st.vfsKeys, nullptr, nullptr,         st.leafDecode};
    traversePathTree(ctx, job.node, job.nodeEnd, job.accumulated, job.depth);
    st.buffers[slot] = std::move(local);
}

/// Traverse a small whole sub-manifest blob fully into its slot.
static void traverseSmallManifestJob(ParallelTraverseState& st, const SubManifestJob& job,
                                     size_t slot) {
    std::vector<RootEntry> local;
    auto subData = (*st.resolver)(std::span<const u8>(job.eKey.data(), kTvfsEKeySize));
    if (!subData.empty()) {
        TvfsHeader subHdr;
        if (parseHeader(subData, subHdr)) {
            u32 const subCftOffsSize = getCftOffsSize(subHdr.cftTableSize);
            TraversalCtx ctx{subData,    subHdr,  subCftOffsSize, local,        st.resolver,
                             st.vfsKeys, nullptr, nullptr,        st.leafDecode};
            parsePathTable(ctx, job.containerPath);
        }
    }
    st.buffers[slot] = std::move(local);
}

/// Fill @p outEntries by traversing a TVFS blob. Returns true on success.
///
/// With a resolver + worker pool the traversal runs in two strictly separate
/// phases — no job ever spawns another job (that would risk a worker-pool
/// deadlock):
///   1. The *driver* thread descends the path tree, splitting any folder
///      bigger than kSubtreeSplitBytes into smaller folders and resolving
///      sub-manifests, until it has a flat list of small, balanced subtree
///      jobs. This descent only walks the "spine" of big folders, so it's
///      cheap even though it's single-threaded.
///   2. Every accumulated job is dispatched in one flat batch and traversed
///      into a private buffer; the buffers are concatenated at the end.
/// Without a pool it falls back to plain recursion.
bool traverseTvfsBlob(std::span<const u8> data, const VfsResolver* resolver,
                      const std::vector<std::array<u8, 16>>* vfsEKeys,
                      std::vector<RootEntry>& outEntries, interfaces::WorkerPool* pool,
                      TvfsLeafDecode leafDecode) {
    TvfsHeader hdr;
    if (!parseHeader(data, hdr))
        return false;

    // Each CFT entry is eKeySize bytes (eKeySize*2 with CKeys).
    if (hdr.eKeySize > 0)
        outEntries.reserve(hdr.cftTableSize / hdr.eKeySize);

    u32 const cftOffsSize = getCftOffsSize(hdr.cftTableSize);

    // isVfsSubManifest binary-searches this set, so pack + sort it up front and
    // point every traversal context at it instead of the caller's order.
    VfsKeySet vfsKeySet;
    const VfsKeySet* vfsKeys = nullptr;
    if (vfsEKeys && !vfsEKeys->empty()) {
        vfsKeySet.build(*vfsEKeys);
        vfsKeys = &vfsKeySet;
    }

    // No sub-container resolution, or no pool — single-threaded recursion.
    if (!resolver || !vfsKeys || !pool) {
        // Everything lands in one vector here, so size it before the walk: the
        // parallel path gets an exact count for free when it concatenates its
        // per-job buffers, but growing into 3.2M entries costs ~240 ms. The VFS
        // table holds one record per file, so its byte size over the record
        // stride lands within 0.1% of the real count on WoW retail.
        if (resolver && vfsEKeys) {
            auto entriesIn = [](const TvfsHeader& h) {
                return h.vfsTableSize / (1 + 4 + 4 + getCftOffsSize(h.cftTableSize));
            };
            size_t predicted = entriesIn(hdr);
            for (auto& k : *vfsEKeys) {
                auto blob = (*resolver)(std::span<const u8>(k.data(), kTvfsEKeySize));
                TvfsHeader subHdr;
                if (!blob.empty() && parseHeader(blob, subHdr))
                    predicted += entriesIn(subHdr);
            }
            outEntries.reserve(predicted);
        }
        TraversalCtx ctx{data,    hdr,     cftOffsSize, outEntries, resolver,
                         vfsKeys, nullptr, nullptr,     leafDecode};
        parsePathTable(ctx);
        return !outEntries.empty();
    }

    std::vector<SubManifestJob> manifestRefs;   // sub-containers as discovered
    std::vector<SubtreeJob> worklist;           // folders to split or accept
    std::vector<SubtreeJob> readyJobs;          // small-enough subtrees → jobs
    std::vector<SubManifestJob> smallManifests; // whole-blob jobs

    // One-level walk of a blob: emits this level's leaves into outEntries,
    // queues folder children onto the worklist, queues sub-containers as refs.
    auto seedBlob = [&](std::span<const u8> bd, const TvfsHeader& bh, u32 bc,
                        const std::string& prefix) {
        TraversalCtx ctx{bd,        bh,        bc, outEntries, resolver, vfsKeys, &manifestRefs,
                         &worklist, leafDecode};
        parsePathTable(ctx, prefix);
    };

    // Phase 1a (driver thread): fully traverse the *root* blob. It's small (a
    // manifest-of-manifests), but a small folder there can reference a giant
    // sub-manifest — the byte-size split heuristic can't see through that — so
    // the root blob must be walked completely to collect every sub-container
    // ref. Sub-containers are deferred (pendingJobs); folders recurse inline.
    {
        TraversalCtx ctx{data,      hdr,     cftOffsSize,   outEntries,
                         resolver,  vfsKeys, &manifestRefs, /*pendingSubtrees=*/nullptr,
                         leafDecode};
        parsePathTable(ctx);
    }

    // Phase 1b (driver thread): resolve sub-manifests and descend/split the
    // big ones into small balanced jobs. worklist/manifestRefs both grow as we
    // go (folders reveal sub-folders; big folders reveal sub-containers), so
    // loop until both are drained.
    size_t wCursor = 0, mCursor = 0;
    while (wCursor < worklist.size() || mCursor < manifestRefs.size()) {
        // Resolve newly-discovered sub-manifests. Big ones are seeded into the
        // worklist (descended like any folder); small ones become whole-blob jobs.
        while (mCursor < manifestRefs.size()) {
            SubManifestJob ref = std::move(manifestRefs[mCursor++]);
            auto blob = (*resolver)(std::span<const u8>(ref.eKey.data(), kTvfsEKeySize));
            if (blob.empty())
                continue;
            TvfsHeader subHdr;
            if (!parseHeader(blob, subHdr))
                continue;
            u32 const approxEntries = subHdr.eKeySize ? subHdr.cftTableSize / subHdr.eKeySize : 0;
            if (approxEntries < kBigBlobMinEntries) {
                smallManifests.push_back(std::move(ref));
                continue;
            }
            // The resolver owns the blob for the whole traversal, so the span
            // stays valid through phase 2.
            seedBlob(blob, subHdr, getCftOffsSize(subHdr.cftTableSize), ref.containerPath);
        }
        // Split big folders; accept small ones as jobs.
        while (wCursor < worklist.size()) {
            SubtreeJob job = std::move(worklist[wCursor++]);
            size_t const bytes = static_cast<size_t>(job.nodeEnd - job.node);
            if (bytes <= kSubtreeSplitBytes || job.depth >= kMaxSplitDepth) {
                readyJobs.push_back(std::move(job));
                continue;
            }
            TraversalCtx ctx{job.data, job.hdr,       job.cftOffsSize, outEntries, resolver,
                             vfsKeys,  &manifestRefs, &worklist,       leafDecode};
            traversePathTree(ctx, job.node, job.nodeEnd, job.accumulated, job.depth);
        }
    }
    if (readyJobs.empty() && smallManifests.empty())
        return !outEntries.empty();

    // Phase 2: one flat parallel batch — each job traverses its (already
    // small) subtree fully into a private buffer. No job spawns work.
    ParallelTraverseState st;
    st.resolver = resolver;
    st.vfsKeys = vfsKeys;
    st.leafDecode = leafDecode;
    st.buffers.resize(readyJobs.size() + smallManifests.size());
    {
        utils::JobGroup jobGroup;
        jobGroup.add(readyJobs.size() + smallManifests.size());
        for (size_t i = 0; i < readyJobs.size(); ++i) {
            interfaces::WorkerTask task;
            task.fn = [&st, &jobGroup, &readyJobs, i]() {
                traverseSubtreeJob(st, readyJobs[i], i);
                jobGroup.done();
            };
            pool->submit(task);
        }
        size_t const manifestBase = readyJobs.size();
        for (size_t i = 0; i < smallManifests.size(); ++i) {
            interfaces::WorkerTask task;
            task.fn = [&st, &jobGroup, &smallManifests, i, manifestBase]() {
                traverseSmallManifestJob(st, smallManifests[i], manifestBase + i);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    }

    // Phase 3: concatenate in job order.
    size_t total = outEntries.size();
    for (auto& buf : st.buffers)
        total += buf.size();
    outEntries.reserve(total);
    for (auto& buf : st.buffers)
        outEntries.insert(outEntries.end(), std::make_move_iterator(buf.begin()),
                          std::make_move_iterator(buf.end()));

    return !outEntries.empty();
}

} // namespace

std::unique_ptr<TvfsRoot> TvfsRoot::parse(std::span<const u8> data, interfaces::WorkerPool* pool,
                                          bool buildIdx, TvfsLeafDecode leafDecode) {
    auto root = std::make_unique<TvfsRoot>();
    root->m_leafDecode = leafDecode;
    if (!traverseTvfsBlob(data, nullptr, nullptr, root->m_entries, pool, leafDecode))
        return nullptr;
    if (buildIdx)
        root->buildIndices(pool, /*preNormalized=*/true);
    return root;
}

std::unique_ptr<TvfsRoot> TvfsRoot::parse(std::span<const u8> data, const VfsResolver& resolver,
                                          const std::vector<std::array<u8, 16>>& vfsEKeys,
                                          interfaces::WorkerPool* pool, bool buildIdx,
                                          TvfsLeafDecode leafDecode) {
    auto root = std::make_unique<TvfsRoot>();
    root->m_leafDecode = leafDecode;
    if (!traverseTvfsBlob(data, &resolver, &vfsEKeys, root->m_entries, pool, leafDecode))
        return nullptr;
    if (buildIdx)
        root->buildIndices(pool, /*preNormalized=*/true);
    return root;
}

void TvfsRoot::ensureIndexed(interfaces::WorkerPool* pool) {
    if (m_byPathMap.size() != 0 || m_entries.empty())
        return;
    buildIndices(pool, /*preNormalized=*/true);
}

std::vector<RootEntry> TvfsRoot::takeEntries() {
    m_byPathMap = {};
    m_chainNext.clear();
    m_trie.clear();
    return std::move(m_entries);
}

void TvfsRoot::merge(const TvfsRoot& other) {
    size_t const base = m_entries.size();
    m_entries.insert(m_entries.end(), other.m_entries.begin(), other.m_entries.end());
    m_chainNext.resize(m_entries.size(), kNoChain);
    for (size_t i = base; i < m_entries.size(); ++i) {
        if (m_entries[i].path.empty())
            continue;
        auto normalized = normalizeCascPath(m_entries[i].path);
        m_entries[i].path = normalized;
        u64 const h = pathHash64(normalized);
        auto* head = m_byPathMap.find(h);
        if (head) {
            m_chainNext[i] = *head;
            m_byPathMap.insertOrAssign(h, static_cast<u32>(i));
        } else {
            m_byPathMap.emplace(h, static_cast<u32>(i));
        }
    }
    m_trie.clear(); // invalidate — rebuilt lazily on next enumerateUnder()
}

std::vector<const RootEntry*> TvfsRoot::findByPath(const std::string& path) const {
    return findByNormalizedPath(normalizeCascPath(path));
}

std::vector<const RootEntry*> TvfsRoot::findByNormalizedPath(
    const std::string& normalizedPath) const {
    std::vector<const RootEntry*> results;
    auto* head = m_byPathMap.find(pathHash64(normalizedPath));
    if (!head)
        return results;
    for (u32 idx = *head; idx != kNoChain; idx = m_chainNext[idx]) {
        if (m_entries[idx].path == normalizedPath)
            results.push_back(&m_entries[idx]);
    }
    return results;
}

bool TvfsRoot::hasPath(const std::string& normalizedPath) const {
    auto* head = m_byPathMap.find(pathHash64(normalizedPath));
    if (!head)
        return false;
    for (u32 idx = *head; idx != kNoChain; idx = m_chainNext[idx]) {
        if (m_entries[idx].path == normalizedPath)
            return true;
    }
    return false;
}

std::vector<const RootEntry*> TvfsRoot::findByFileDataId(u32 /*fileDataId*/,
                                                         FileIdHint /*hint*/) const {
    return {};
}

void TvfsRoot::enumerateUnder(const std::string& normalizedPrefix,
                              std::function<bool(const RootEntry&)> callback) const {
    if (!callback)
        return;
    ensureTrie();
    u32 const startNode = trieWalkTo(m_trie, normalizedPrefix);
    if (startNode == UINT32_MAX)
        return;
    trieDfs(m_trie, startNode, m_entries, callback);
}

void TvfsRoot::buildIndices(interfaces::WorkerPool* pool, bool preNormalized) {
    size_t const n = m_entries.size();
    m_chainNext.assign(n, kNoChain);
    m_byPathMap.reserve(n);

    if (!preNormalized) {
        // Lowercase + path-separator normalisation in parallel.
        auto lowerPaths = normalizeEntryPaths(m_entries, pool);
        for (size_t i = 0; i < n; ++i)
            m_entries[i].path = std::move(lowerPaths[i]);
    }

    for (size_t i = 0; i < n; ++i) {
        if (m_entries[i].path.empty())
            continue;
        u64 const h = pathHash64(m_entries[i].path);
        auto* head = m_byPathMap.find(h);
        if (head) {
            m_chainNext[i] = *head;
            m_byPathMap.insertOrAssign(h, static_cast<u32>(i));
        } else {
            m_byPathMap.emplace(h, static_cast<u32>(i));
        }
    }

    m_trie.clear(); // built lazily on first enumerateUnder()
}

void TvfsRoot::ensureTrie() const {
    std::call_once(m_trieOnce, [this]() {
        size_t const n = m_entries.size();
        m_trie.reserve(n / 4);
        m_trie.emplace_back();
        for (size_t i = 0; i < n; ++i) {
            if (!m_entries[i].path.empty())
                trieInsert(m_trie, m_entries[i].path, static_cast<u32>(i));
        }
        trieSortAll(m_trie);
    });
}

} // namespace whiteout::storages::casc
