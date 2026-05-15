// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "tvfs_root.h"
#include "common/root_build_utils.h"
#include "../storage/constants.h"
#include "../../common/byte_order.h"
#include "../../common/string_utils.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace whiteout::storages::casc {

using storages::common::readLE32;
using storages::common::readBE16;
using storages::common::readBE32;
using storages::common::readBEVar;
using storages::common::normalizeCascPath;

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

// ---- FNV-1a string hashing with MurmurHash3 finalizer ----

/// FNV-1a 64-bit hash of a byte range.
static u64 fnv1a64(const char* data, size_t len) {
    u64 h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<u64>(static_cast<u8>(data[i]));
        h *= 1099511628211ULL;
    }
    return h;
}

/// MurmurHash3 64-bit finalizer — mixes bits so low bits are well-distributed
/// for power-of-2 masking in FlatHashMap.
static u64 mixBits(u64 h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

/// Hash a path string for use as FlatHashMap key.
/// Guaranteed non-zero (FlatHashMap uses 0 as empty sentinel).
static u64 pathHash64(const std::string& s) {
    u64 h = mixBits(fnv1a64(s.data(), s.size()));
    return h == 0 ? 1 : h;
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
    u32 flags = 0;            // LE!
    u32 pathTableOffset = 0;  // BE
    u32 pathTableSize = 0;    // BE
    u32 vfsTableOffset = 0;   // BE
    u32 vfsTableSize = 0;     // BE
    u32 cftTableOffset = 0;   // BE
    u32 cftTableSize = 0;     // BE
    u16 maxDepth = 0;         // BE
    u32 estTableOffset = 0;   // BE
    u32 estTableSize = 0;     // BE
};

/// Minimum header size: sig(4)+ver(1)+hdrSz(1)+eKey(1)+pKey(1)
///   +flags(4)+6*off/sz(24)+maxDepth(2)+2*est(8) = 46 bytes.
// kTvfsMinHeaderSize is defined in constants.h.

static bool parseHeader(std::span<const u8> data, TvfsHeader& hdr) {
    if (data.size() < kTvfsMinHeaderSize) return false;

    const u8* p = data.data();
    hdr.signature = readLE32(p);
    if (hdr.signature != RootSignature::kTVFS) return false;

    hdr.formatVersion = p[4];
    if (hdr.formatVersion != kTvfsFormatVersion) return false;

    hdr.headerSize = p[5];
    hdr.eKeySize = p[6];
    hdr.patchKeySize = p[7];

    // CascLib enforces eKeySize == 9 (ERROR_BAD_FORMAT otherwise).
    // All known TVFS manifests use 9-byte eKeys matching CDN index key size.
    if (hdr.eKeySize != kTvfsEKeySize) return false;

    hdr.flags = readLE32(p + 8);
    hdr.pathTableOffset  = readBE32(p + 12);
    hdr.pathTableSize    = readBE32(p + 16);
    hdr.vfsTableOffset   = readBE32(p + 20);
    hdr.vfsTableSize     = readBE32(p + 24);
    hdr.cftTableOffset   = readBE32(p + 28);
    hdr.cftTableSize     = readBE32(p + 32);
    hdr.maxDepth         = readBE16(p + 36);
    hdr.estTableOffset   = readBE32(p + 38);
    hdr.estTableSize     = readBE32(p + 42);

    // Sanity check offsets within data.
    if (hdr.pathTableOffset + hdr.pathTableSize > data.size()) return false;
    if (hdr.vfsTableOffset + hdr.vfsTableSize > data.size()) return false;
    if (hdr.cftTableOffset + hdr.cftTableSize > data.size()) return false;

    return true;
}

/// Determine byte-size of a CFT offset field based on CFT table size.
static u32 getCftOffsSize(u32 cftTableSize) {
    if (cftTableSize <= 0xFF) return 1;
    if (cftTableSize <= 0xFFFF) return 2;
    if (cftTableSize <= 0xFFFFFF) return 3;
    return 4;
}

// ============================================================================
// Path table traversal
// ============================================================================

/// A deferred sub-manifest traversal — queued during the root-blob pass so the
/// independent sub-manifests can be traversed in parallel.
struct SubManifestJob {
    std::array<u8, 16> eKey{};   ///< Zero-padded; first kTvfsEKeySize bytes valid.
    std::string containerPath;   ///< Path prefix ("parent:") for this sub-manifest.
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
    std::string accumulated;     ///< Path accumulated down to this folder.
    int depth = 0;
};

/// Context for recursive path-tree traversal.
struct TraversalCtx {
    std::span<const u8> data;
    const TvfsHeader& hdr;
    u32 cftOffsSize;
    std::vector<RootEntry>& entries;
    const VfsResolver* resolver;                            ///< null when no sub-container resolution.
    const std::vector<std::array<u8, 16>>* vfsEKeys;       ///< null when no sub-container resolution.
    /// When non-null, sub-containers are queued here instead of recursed into
    /// inline — lets the caller traverse them in parallel. Null in job contexts
    /// (nested sub-containers recurse inline within their parent job).
    std::vector<SubManifestJob>* pendingJobs = nullptr;
    /// When non-null, folder nodes are queued here instead of recursed into —
    /// turns traversePathTree into a single-level walk so the caller can
    /// descend the tree breadth-first and farm out subtrees in parallel.
    std::vector<SubtreeJob>* pendingSubtrees = nullptr;
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

static bool isVfsSubManifest(const TraversalCtx& ctx, const u8* eKey) {
    if (!ctx.vfsEKeys) return false;
    for (auto& vfsEKey : *ctx.vfsEKeys) {
        if (std::memcmp(vfsEKey.data(), eKey, ctx.hdr.eKeySize) == 0)
            return true;
    }
    return false;
}

/// If a single-span entry resolves to a known VFS sub-manifest, emit a
/// container entry, recurse into the sub-manifest with a `path:` prefix,
/// and return true. Returns false to fall through to the normal-entry path.
static bool tryEmitSubContainer(TraversalCtx& ctx,
                                 const std::string& path,
                                 size_t vfsPos,
                                 u32 spanEntrySize,
                                 bool hasCKey) {
    if (!ctx.resolver || !ctx.vfsEKeys) return false;

    auto& hdr = ctx.hdr;
    const u8* vfsBase = ctx.data.data() + hdr.vfsTableOffset;
    const u8* cftBase = ctx.data.data() + hdr.cftTableOffset;

    if (vfsPos + spanEntrySize > hdr.vfsTableSize) return false;

    u32 cftOffset = readBEVar(vfsBase + vfsPos + 8, ctx.cftOffsSize);
    u32 cftEntrySize = hasCKey ? (hdr.eKeySize * 2) : hdr.eKeySize;
    if (cftOffset + cftEntrySize > hdr.cftTableSize) return false;

    const u8* eKeyPtr = cftBase + cftOffset;
    if (!isVfsSubManifest(ctx, eKeyPtr)) return false;

    // CascLib uses ':' to separate the sub-container from its parent path.
    std::string containerPath = path + ":";

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
            u32 subCftOffsSize = getCftOffsSize(subHdr.cftTableSize);
            TraversalCtx subCtx{subData, subHdr, subCftOffsSize, ctx.entries,
                                ctx.resolver, ctx.vfsEKeys};
            parsePathTable(subCtx, containerPath);
        }
    }
    return true;
}

/// Leaf node: resolve VFS → CFT → EKey [+ CKey], emit one entry per span.
static void processFileEntry(TraversalCtx& ctx, const std::string& path, u32 vfsOffset) {
    auto& hdr = ctx.hdr;
    const u8* vfsBase = ctx.data.data() + hdr.vfsTableOffset;
    const u8* cftBase = ctx.data.data() + hdr.cftTableOffset;

    if (vfsOffset >= hdr.vfsTableSize) return;

    // VFS entry: SpanCount(u8), then per span: FileOffset(u32 BE) + SpanSize(u32 BE) + CftOffset(var BE).
    u8 spanCount = vfsBase[vfsOffset];
    if (spanCount == 0 || spanCount > 224) return;

    size_t vfsPos = vfsOffset + 1;
    u32 spanEntrySize = 4 + 4 + ctx.cftOffsSize;
    bool hasCKey = (hdr.flags & kTvfsFlagIncludeCKey) != 0;

    if (spanCount == 1 && tryEmitSubContainer(ctx, path, vfsPos, spanEntrySize, hasCKey))
        return;

    for (u8 s = 0; s < spanCount; ++s) {
        if (vfsPos + spanEntrySize > hdr.vfsTableSize) return;

        u32 cftOffset = readBEVar(vfsBase + vfsPos + 8, ctx.cftOffsSize);
        vfsPos += spanEntrySize;

        // CFT entry: EKey(eKeySize) [+ CKey(eKeySize) if IncludeCKey].
        u32 cftEntrySize = hasCKey ? (hdr.eKeySize * 2) : hdr.eKeySize;
        if (cftOffset + cftEntrySize > hdr.cftTableSize) continue;

        RootEntry entry;
        entry.path = path;
        copyKey16(entry.eKey, cftBase + cftOffset, hdr.eKeySize);
        if (hasCKey)
            copyKey16(entry.cKey, cftBase + cftOffset + hdr.eKeySize, hdr.eKeySize);
        // else: cKey stays zero — resolution must use eKey directly.

        ctx.entries.push_back(std::move(entry));
    }
}

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
    if (depth > kTvfsMaxTraversalDepth) return; // safety limit

    std::string pathBuf = accumulated;

    while (node < nodeEnd) {
        // --- CapturePathEntry equivalent ---

        // Pre-0x00: path separator before name.
        if (*node == kTvfsPathSeparator) {
            pathBuf += '\\';
            ++node;
            if (node >= nodeEnd) break;
        }

        // Name fragment: [len][bytes].  Lowercase in-place so the path is
        // already normalized for lookup (avoids a post-pass over all entries).
        std::string name;
        if (node < nodeEnd && *node != kTvfsNodeValueMarker) {
            u8 nameLen = *node++;
            if (node + nameLen > nodeEnd) return;
            if (nameLen > 0) {
                name.assign(reinterpret_cast<const char*>(node), nameLen);
                for (auto& c : name) {
                    if (c == '/') c = '\\';
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                node += nameLen;
            }
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
                if (node + 4 > nodeEnd) break;
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
        pathBuf += name;
        if (hasPostSep)
            pathBuf += '\\';

        // --- Process node value ---
        if (hasNodeValue) {
            if (nodeValue & kTvfsFolderNodeBit) {
                // Folder node: recurse into children.
                u32 folderDataLen = nodeValue & ~kTvfsFolderNodeBit;
                if (folderDataLen < 4) break;
                u32 innerLen = folderDataLen - 4;
                if (node + innerLen > nodeEnd) break;

                if (ctx.pendingSubtrees) {
                    // Single-level mode: defer this folder's subtree.
                    ctx.pendingSubtrees->push_back(
                        SubtreeJob{ctx.data, ctx.hdr, ctx.cftOffsSize,
                                   node, node + innerLen, pathBuf, depth + 1});
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
                while (s < pathBuf.size() && pathBuf[s] == '\\') ++s;
                size_t e = pathBuf.size();
                while (e > s && pathBuf[e - 1] == '\\') --e;
                if (s > 0 || e < pathBuf.size())
                    processFileEntry(ctx, pathBuf.substr(s, e - s), nodeValue);
                else
                    processFileEntry(ctx, pathBuf, nodeValue);
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
        if (node + 5 > nodeEnd) return;
        u32 nodeValue = readBE32(node + 1);
        if (nodeValue & kTvfsFolderNodeBit) {
            u32 folderDataLen = nodeValue & ~kTvfsFolderNodeBit;
            if (folderDataLen < 4) return;
            u32 innerLen = folderDataLen - 4;
            const u8* childStart = node + 5; // past 0xFF + 4-byte value
            const u8* childEnd = childStart + innerLen;
            if (childEnd > nodeEnd) return;
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
    const std::vector<std::array<u8, 16>>* vfsEKeys = nullptr;
    std::mutex bufMutex;
    std::vector<std::vector<RootEntry>> buffers;

    void emit(std::vector<RootEntry>&& local) {
        if (local.empty()) return;
        std::lock_guard<std::mutex> lk(bufMutex);
        buffers.push_back(std::move(local));
    }
};

/// Traverse one (already small-enough) folder subtree fully into a buffer.
static void traverseSubtreeJob(ParallelTraverseState& st, const SubtreeJob& job) {
    std::vector<RootEntry> local;
    TraversalCtx ctx{job.data, job.hdr, job.cftOffsSize, local,
                     st.resolver, st.vfsEKeys};
    traversePathTree(ctx, job.node, job.nodeEnd, job.accumulated, job.depth);
    st.emit(std::move(local));
}

/// Traverse a small whole sub-manifest blob fully into a buffer.
static void traverseSmallManifestJob(ParallelTraverseState& st, const SubManifestJob& job) {
    std::vector<RootEntry> local;
    auto subData = (*st.resolver)(
        std::span<const u8>(job.eKey.data(), kTvfsEKeySize));
    if (!subData.empty()) {
        TvfsHeader subHdr;
        if (parseHeader(subData, subHdr)) {
            u32 subCftOffsSize = getCftOffsSize(subHdr.cftTableSize);
            TraversalCtx ctx{subData, subHdr, subCftOffsSize, local,
                             st.resolver, st.vfsEKeys};
            parsePathTable(ctx, job.containerPath);
        }
    }
    st.emit(std::move(local));
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
bool traverseTvfsBlob(std::span<const u8> data,
                       const VfsResolver* resolver,
                       const std::vector<std::array<u8, 16>>* vfsEKeys,
                       std::vector<RootEntry>& outEntries,
                       interfaces::WorkerPool* pool) {
    TvfsHeader hdr;
    if (!parseHeader(data, hdr))
        return false;

    // Each CFT entry is eKeySize bytes (eKeySize*2 with CKeys).
    if (hdr.eKeySize > 0)
        outEntries.reserve(hdr.cftTableSize / hdr.eKeySize);

    u32 cftOffsSize = getCftOffsSize(hdr.cftTableSize);

    // No sub-container resolution, or no pool — single-threaded recursion.
    if (!resolver || !vfsEKeys || !pool) {
        TraversalCtx ctx{data, hdr, cftOffsSize, outEntries, resolver, vfsEKeys};
        parsePathTable(ctx);
        return !outEntries.empty();
    }

    std::vector<SubManifestJob> manifestRefs;   // sub-containers as discovered
    std::vector<SubtreeJob> worklist;           // folders to split or accept
    std::vector<SubtreeJob> readyJobs;          // small-enough subtrees → jobs
    std::vector<SubManifestJob> smallManifests; // whole-blob jobs
    std::vector<std::vector<u8>> resolvedBlobs; // keeps big sub-manifest data alive

    // One-level walk of a blob: emits this level's leaves into outEntries,
    // queues folder children onto the worklist, queues sub-containers as refs.
    auto seedBlob = [&](std::span<const u8> bd, const TvfsHeader& bh, u32 bc,
                        const std::string& prefix) {
        TraversalCtx ctx{bd, bh, bc, outEntries, resolver, vfsEKeys,
                         &manifestRefs, &worklist};
        parsePathTable(ctx, prefix);
    };

    // Phase 1a (driver thread): fully traverse the *root* blob. It's small (a
    // manifest-of-manifests), but a small folder there can reference a giant
    // sub-manifest — the byte-size split heuristic can't see through that — so
    // the root blob must be walked completely to collect every sub-container
    // ref. Sub-containers are deferred (pendingJobs); folders recurse inline.
    {
        TraversalCtx ctx{data, hdr, cftOffsSize, outEntries, resolver, vfsEKeys,
                         &manifestRefs, /*pendingSubtrees=*/nullptr};
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
            auto blob = (*resolver)(
                std::span<const u8>(ref.eKey.data(), kTvfsEKeySize));
            if (blob.empty()) continue;
            TvfsHeader subHdr;
            if (!parseHeader(blob, subHdr)) continue;
            u32 approxEntries = subHdr.eKeySize
                ? subHdr.cftTableSize / subHdr.eKeySize : 0;
            if (approxEntries < kBigBlobMinEntries) {
                smallManifests.push_back(std::move(ref));
                continue;
            }
            resolvedBlobs.push_back(std::move(blob));
            // Moving the outer vector relocates the inner vector objects but
            // not their heap buffers, so the span stays valid for phase 2.
            seedBlob(resolvedBlobs.back(), subHdr,
                     getCftOffsSize(subHdr.cftTableSize), ref.containerPath);
        }
        // Split big folders; accept small ones as jobs.
        while (wCursor < worklist.size()) {
            SubtreeJob job = std::move(worklist[wCursor++]);
            size_t bytes = static_cast<size_t>(job.nodeEnd - job.node);
            if (bytes <= kSubtreeSplitBytes || job.depth >= kMaxSplitDepth) {
                readyJobs.push_back(std::move(job));
                continue;
            }
            TraversalCtx ctx{job.data, job.hdr, job.cftOffsSize, outEntries,
                             resolver, vfsEKeys, &manifestRefs, &worklist};
            traversePathTree(ctx, job.node, job.nodeEnd,
                             job.accumulated, job.depth);
        }
    }
    if (readyJobs.empty() && smallManifests.empty())
        return !outEntries.empty();

    // Phase 2: one flat parallel batch — each job traverses its (already
    // small) subtree fully into a private buffer. No job spawns work.
    ParallelTraverseState st;
    st.resolver = resolver;
    st.vfsEKeys = vfsEKeys;
    {
        utils::JobGroup jobGroup;
        jobGroup.add(readyJobs.size() + smallManifests.size());
        for (size_t i = 0; i < readyJobs.size(); ++i) {
            interfaces::WorkerTask task;
            task.fn = [&st, &jobGroup, &readyJobs, i]() {
                traverseSubtreeJob(st, readyJobs[i]);
                jobGroup.done();
            };
            pool->submit(task);
        }
        for (size_t i = 0; i < smallManifests.size(); ++i) {
            interfaces::WorkerTask task;
            task.fn = [&st, &jobGroup, &smallManifests, i]() {
                traverseSmallManifestJob(st, smallManifests[i]);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    }

    // Phase 3: concatenate. Entry order is not load-bearing — every consumer
    // (index build, enumerate, merge) is order-agnostic.
    size_t total = outEntries.size();
    for (auto& buf : st.buffers) total += buf.size();
    outEntries.reserve(total);
    for (auto& buf : st.buffers)
        outEntries.insert(outEntries.end(),
                          std::make_move_iterator(buf.begin()),
                          std::make_move_iterator(buf.end()));

    return !outEntries.empty();
}

} // namespace

std::unique_ptr<TvfsRoot> TvfsRoot::parse(
    std::span<const u8> data,
    interfaces::WorkerPool* pool,
    bool buildIdx)
{
    auto root = std::make_unique<TvfsRoot>();
    if (!traverseTvfsBlob(data, nullptr, nullptr, root->m_entries, pool))
        return nullptr;
    if (buildIdx)
        root->buildIndices(pool, /*preNormalized=*/true);
    return root;
}

std::unique_ptr<TvfsRoot> TvfsRoot::parse(
    std::span<const u8> data,
    const VfsResolver& resolver,
    const std::vector<std::array<u8, 16>>& vfsEKeys,
    interfaces::WorkerPool* pool,
    bool buildIdx)
{
    auto root = std::make_unique<TvfsRoot>();
    if (!traverseTvfsBlob(data, &resolver, &vfsEKeys, root->m_entries, pool))
        return nullptr;
    if (buildIdx)
        root->buildIndices(pool, /*preNormalized=*/true);
    return root;
}

void TvfsRoot::ensureIndexed(interfaces::WorkerPool* pool) {
    if (m_byPathMap.size() != 0 || m_entries.empty()) return;
    buildIndices(pool, /*preNormalized=*/true);
}

std::vector<RootEntry> TvfsRoot::takeEntries() {
    m_byPathMap = {};
    m_chainNext.clear();
    m_trie.clear();
    return std::move(m_entries);
}

void TvfsRoot::merge(const TvfsRoot& other) {
    size_t base = m_entries.size();
    m_entries.insert(m_entries.end(), other.m_entries.begin(), other.m_entries.end());
    m_chainNext.resize(m_entries.size(), kNoChain);
    for (size_t i = base; i < m_entries.size(); ++i) {
        if (m_entries[i].path.empty()) continue;
        auto normalized = normalizeCascPath(m_entries[i].path);
        m_entries[i].path = normalized;
        u64 h = pathHash64(normalized);
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

std::vector<const RootEntry*> TvfsRoot::findByNormalizedPath(const std::string& normalizedPath) const {
    std::vector<const RootEntry*> results;
    auto* head = m_byPathMap.find(pathHash64(normalizedPath));
    if (!head) return results;
    for (u32 idx = *head; idx != kNoChain; idx = m_chainNext[idx]) {
        if (m_entries[idx].path == normalizedPath)
            results.push_back(&m_entries[idx]);
    }
    return results;
}

bool TvfsRoot::hasPath(const std::string& normalizedPath) const {
    auto* head = m_byPathMap.find(pathHash64(normalizedPath));
    if (!head) return false;
    for (u32 idx = *head; idx != kNoChain; idx = m_chainNext[idx]) {
        if (m_entries[idx].path == normalizedPath)
            return true;
    }
    return false;
}

std::vector<const RootEntry*> TvfsRoot::findByFileDataId(u32 /*fileDataId*/, FileIdHint /*hint*/) const {
    return {};
}

void TvfsRoot::enumerateUnder(const std::string& normalizedPrefix,
                               std::function<bool(const RootEntry&)> callback) const {
    if (!callback) return;
    ensureTrie();
    u32 startNode = trieWalkTo(m_trie, normalizedPrefix);
    if (startNode == UINT32_MAX) return;
    trieDfs(m_trie, startNode, m_entries, callback);
}

void TvfsRoot::buildIndices(interfaces::WorkerPool* pool, bool preNormalized) {
    size_t n = m_entries.size();
    m_chainNext.assign(n, kNoChain);
    m_byPathMap.reserve(n);

    if (!preNormalized) {
        // Lowercase + path-separator normalisation in parallel.
        auto lowerPaths = normalizeEntryPaths(m_entries, pool);
        for (size_t i = 0; i < n; ++i)
            m_entries[i].path = std::move(lowerPaths[i]);
    }

    for (size_t i = 0; i < n; ++i) {
        if (m_entries[i].path.empty()) continue;
        u64 h = pathHash64(m_entries[i].path);
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
    if (!m_trie.empty()) return;
    size_t n = m_entries.size();
    m_trie.reserve(n / 4);
    m_trie.emplace_back();
    for (size_t i = 0; i < n; ++i) {
        if (!m_entries[i].path.empty())
            trieInsert(m_trie, m_entries[i].path, static_cast<u32>(i));
    }
    trieSortAll(m_trie);
}

} // namespace whiteout::storages::casc
