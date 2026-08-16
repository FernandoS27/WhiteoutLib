// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Hand-written CASC entry points the codegen can't express. Both operate on
// the same `Storage*` handle the generated `whiteout_casc_CascStorage_*`
// functions use, so a handle produced here is freed with the generated
// `whiteout_casc_CascStorage_delete`.
//
// Compiled into `whiteout_c` only when CASC is enabled — see
// bindings/c/CMakeLists.txt.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/types.h>

#include "whiteout_c_common.h"
#include "whiteout_casc_progress.h"

namespace {
using whiteout::storages::casc::BatchReadRequest;
using whiteout::storages::casc::BatchReadResult;
using whiteout::storages::casc::FileIdHint;
using whiteout::storages::casc::OnlineOpenOptions;
using whiteout::storages::casc::OpenOptions;
using whiteout::storages::casc::ProgressCallback;
using whiteout::storages::casc::ProgressInfo;
using whiteout::storages::casc::Storage;

/// Adapts the C++ callback to the C function pointer. `object` is a
/// string_view on the C++ side, so it is copied into a bounded buffer to get
/// the NUL terminator a C caller expects; names are keys and filenames, so the
/// truncation limit is never reached in practice.
ProgressCallback wrapProgress(whiteout_casc_progress_fn progress, void* user) {
    if (progress == nullptr) return nullptr;
    return [progress, user](const ProgressInfo& info) {
        char objectBuf[192];
        size_t const n = info.object.size() < sizeof(objectBuf) - 1 ? info.object.size()
                                                                    : sizeof(objectBuf) - 1;
        if (n > 0) std::memcpy(objectBuf, info.object.data(), n);
        objectBuf[n] = '\0';

        whiteout_casc_ProgressInfo cInfo{};
        cInfo.size = uint32_t(sizeof(cInfo));
        cInfo.step = int32_t(info.step);
        cInfo.state = int32_t(info.state);
        cInfo.object = objectBuf;
        cInfo.current = info.current;
        cInfo.total = info.total;
        cInfo.bytesDone = info.bytesDone;
        cInfo.bytesTotal = info.bytesTotal;
        cInfo.stepIndex = info.stepIndex;
        cInfo.stepCount = info.stepCount;
        cInfo.elapsedMs = info.elapsedMs;
        cInfo.overallFraction = info.overallFraction();
        return progress(user, &cInfo) != 0;
    };
}
}  // namespace

extern "C" {

// ── openOnline ─────────────────────────────────────────────────────────────
//
// `OnlineOpenOptions` mixes an interface pointer, a `std::function` progress
// callback and nested value objects, none of which the codegen marshals as a
// param struct. The progress callback is left null here — see
// whiteout_casc_shim_openOnlineWithProgress for the reporting variant.

void* whiteout_casc_shim_openOnline(const char* product, const char* region,
                                    const char* buildKey, void* httpHandle,
                                    const char* cacheDir, uint32_t localeMask,
                                    void* poolHandle) {
    OnlineOpenOptions opts;
    opts.product = product != nullptr ? product : "";
    if (region != nullptr && *region != '\0') opts.region = region;
    if (buildKey != nullptr && *buildKey != '\0') opts.buildKey = buildKey;
    opts.http = reinterpret_cast<whiteout::interfaces::HttpHandler*>(httpHandle);
    if (cacheDir != nullptr && *cacheDir != '\0') opts.cacheDir = cacheDir;
    opts.localeMask = localeMask;
    opts.pool = reinterpret_cast<whiteout::interfaces::WorkerPool*>(poolHandle);

    auto storage = Storage::openOnline(opts);
    if (!storage) return nullptr;
    return new Storage(std::move(*storage));
}

// ── progress ───────────────────────────────────────────────────────────────

const char* whiteout_casc_shim_progressStepName(int32_t step) {
    return whiteout::storages::casc::progressStepName(
        static_cast<whiteout::storages::casc::ProgressStep>(step));
}

void* whiteout_casc_shim_openWithProgress(const char* path, const char* product,
                                          uint32_t localeMask, uint32_t flags,
                                          whiteout_casc_progress_fn progress, void* user,
                                          void* poolHandle) {
    OpenOptions opts;
    opts.path = path != nullptr ? path : "";
    if (product != nullptr && *product != '\0') opts.product = product;
    opts.localeMask = localeMask;
    opts.flags = flags;
    opts.progressCallback = wrapProgress(progress, user);
    opts.pool = reinterpret_cast<whiteout::interfaces::WorkerPool*>(poolHandle);

    auto storage = Storage::open(opts);
    if (!storage) return nullptr;
    return new Storage(std::move(*storage));
}

void* whiteout_casc_shim_openOnlineWithProgress(const char* product, const char* region,
                                                const char* buildKey, void* httpHandle,
                                                const char* cacheDir, uint32_t localeMask,
                                                uint32_t flags,
                                                whiteout_casc_progress_fn progress, void* user,
                                                void* poolHandle) {
    OnlineOpenOptions opts;
    opts.product = product != nullptr ? product : "";
    if (region != nullptr && *region != '\0') opts.region = region;
    if (buildKey != nullptr && *buildKey != '\0') opts.buildKey = buildKey;
    opts.http = reinterpret_cast<whiteout::interfaces::HttpHandler*>(httpHandle);
    if (cacheDir != nullptr && *cacheDir != '\0') opts.cacheDir = cacheDir;
    opts.localeMask = localeMask;
    opts.flags = flags;
    opts.progressCallback = wrapProgress(progress, user);
    opts.pool = reinterpret_cast<whiteout::interfaces::WorkerPool*>(poolHandle);

    auto storage = Storage::openOnline(opts);
    if (!storage) return nullptr;
    return new Storage(std::move(*storage));
}

void whiteout_casc_shim_setProgressCallback(void* self, whiteout_casc_progress_fn progress,
                                            void* user) {
    if (self == nullptr) return;
    reinterpret_cast<Storage*>(self)->setProgressCallback(wrapProgress(progress, user));
}

// ── readBatch ──────────────────────────────────────────────────────────────
//
// `readBatch` takes a span of value objects and returns a vector of them —
// unsupported in both directions. Requests arrive as parallel arrays; results
// are read back through a heap-owned snapshot, so a batch of N files costs one
// native allocation rather than N.
//
// With a WorkerPool configured on the storage, readBatch overlaps resolution,
// raw read and BLTE decode across files; this is the bulk-extraction fast path
// versus calling readFile per file.

/// Run a batch read. Parallel input arrays of length @p count:
///  - paths[i]:       CASC path, or null to read by FileDataId.
///  - fileDataIds[i]: used when paths[i] is null.
///  - hints[i]:       FileIdHint for the FileDataId lookup.
/// Returns an opaque snapshot, or null when @p count is 0. Free via
/// whiteout_casc_shim_readBatch_free.
void* whiteout_casc_shim_readBatch(const void* self, const char* const* paths,
                                   const int32_t* fileDataIds, const int32_t* hints,
                                   size_t count) {
    if (self == nullptr || count == 0) return nullptr;
    const auto* storage = reinterpret_cast<const Storage*>(self);

    std::vector<BatchReadRequest> requests;
    requests.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        BatchReadRequest r;
        if (paths != nullptr && paths[i] != nullptr) {
            r.path = paths[i];
        } else {
            r.fileDataId = fileDataIds != nullptr ? fileDataIds[i] : -1;
            r.fileIdHint = static_cast<FileIdHint>(hints != nullptr ? hints[i] : 0);
        }
        requests.push_back(std::move(r));
    }

    return new std::vector<BatchReadResult>(storage->readBatch(requests));
}

size_t whiteout_casc_shim_readBatch_count(void* snapshot) {
    if (snapshot == nullptr) return 0;
    return reinterpret_cast<std::vector<BatchReadResult>*>(snapshot)->size();
}

/// Borrowed view into the snapshot's i-th buffer — `_owner` is null, so the
/// caller must copy without freeing. Valid until the snapshot is freed.
whiteout_Bytes whiteout_casc_shim_readBatch_data_at(void* snapshot, size_t index) {
    auto* results = reinterpret_cast<std::vector<BatchReadResult>*>(snapshot);
    if (results == nullptr || index >= results->size())
        return whiteout_Bytes{nullptr, 0, nullptr};
    const auto& data = (*results)[index].data;
    if (data.empty()) return whiteout_Bytes{nullptr, 0, nullptr};
    return whiteout_Bytes{data.data(), data.size(), nullptr};
}

int32_t whiteout_casc_shim_readBatch_success_at(void* snapshot, size_t index) {
    auto* results = reinterpret_cast<std::vector<BatchReadResult>*>(snapshot);
    if (results == nullptr || index >= results->size()) return 0;
    return (*results)[index].success ? 1 : 0;
}

/// Borrowed view into the snapshot's i-th diagnostic message; same ownership
/// rules as readBatch_data_at.
whiteout_CString whiteout_casc_shim_readBatch_error_at(void* snapshot, size_t index) {
    auto* results = reinterpret_cast<std::vector<BatchReadResult>*>(snapshot);
    if (results == nullptr || index >= results->size())
        return whiteout_CString{nullptr, 0, nullptr};
    const auto& error = (*results)[index].error;
    return whiteout_CString{error.c_str(), error.size(), nullptr};
}

void whiteout_casc_shim_readBatch_free(void* snapshot) {
    delete reinterpret_cast<std::vector<BatchReadResult>*>(snapshot);
}

}  // extern "C"
