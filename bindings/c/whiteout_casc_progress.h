/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 Fernando Sahmkow */
/* HAND-WRITTEN (not codegen). Progress reporting for CASC storage opens —
 * the C++ ProgressCallback is a std::function taking a struct, neither of
 * which the codegen marshals. */

#ifndef WHITEOUT_CASC_PROGRESS_H
#define WHITEOUT_CASC_PROGRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors whiteout::storages::casc::ProgressStep. */
typedef enum {
    WHITEOUT_CASC_STEP_RESOLVING_VERSION = 0,
    WHITEOUT_CASC_STEP_LOADING_BUILD_CONFIG = 1,
    WHITEOUT_CASC_STEP_LOADING_CDN_CONFIG = 2,
    WHITEOUT_CASC_STEP_LOADING_INDEX_FILES = 3,
    WHITEOUT_CASC_STEP_MAPPING_ARCHIVES = 4,
    WHITEOUT_CASC_STEP_LOADING_ARCHIVE_INDEXES = 5,
    WHITEOUT_CASC_STEP_LOADING_ENCODING_TABLE = 6,
    WHITEOUT_CASC_STEP_LOADING_VFS_MANIFESTS = 7,
    WHITEOUT_CASC_STEP_LOADING_ROOT_MANIFEST = 8,
    WHITEOUT_CASC_STEP_READY = 9
} whiteout_casc_ProgressStep;

/* Mirrors whiteout::storages::casc::ProgressState. */
typedef enum {
    WHITEOUT_CASC_PROGRESS_BEGIN = 0,
    WHITEOUT_CASC_PROGRESS_UPDATE = 1,
    WHITEOUT_CASC_PROGRESS_END = 2
} whiteout_casc_ProgressState;

/* One progress event. `size` is set to sizeof(whiteout_casc_ProgressInfo) by
 * the library so a caller built against an older header can tell which trailing
 * fields are present. */
typedef struct {
    uint32_t size;
    int32_t step;  /* whiteout_casc_ProgressStep  */
    int32_t state; /* whiteout_casc_ProgressState */
    int32_t _pad;
    /* UTF-8, NUL-terminated, borrowed for the duration of the call. Never null;
     * an event with no object name passes "". Long names are truncated. */
    const char* object;
    uint64_t current;
    uint64_t total;
    uint64_t bytesDone;
    uint64_t bytesTotal;
    uint32_t stepIndex;
    uint32_t stepCount;
    double elapsedMs;
    double overallFraction; /* [0,1] across the whole operation */
} whiteout_casc_ProgressInfo;

/* Return 0 to cancel the operation, non-zero to continue.
 * May be invoked from a worker thread, but never concurrently. */
typedef int32_t (*whiteout_casc_progress_fn)(void* user,
                                             const whiteout_casc_ProgressInfo* info);

/* English label for a step; never null. */
const char* whiteout_casc_shim_progressStepName(int32_t step);

/* Open a local storage with progress reporting. `product` and `progress` may be
 * null. Returns a Storage* freed with whiteout_casc_CascStorage_delete, or null
 * on failure (including cancellation — check whiteout_casc_CascStorage_lastError). */
void* whiteout_casc_shim_openWithProgress(const char* path, const char* product,
                                          uint32_t localeMask, uint32_t flags,
                                          whiteout_casc_progress_fn progress, void* user,
                                          void* poolHandle);

/* openOnline plus feature flags and progress. Same ownership as
 * whiteout_casc_shim_openOnline. */
void* whiteout_casc_shim_openOnlineWithProgress(const char* product, const char* region,
                                                const char* buildKey, void* httpHandle,
                                                const char* cacheDir, uint32_t localeMask,
                                                uint32_t flags,
                                                whiteout_casc_progress_fn progress, void* user,
                                                void* poolHandle);

/* Install a callback for work that happens after open: the deferred load of a
 * LoadOnDemand storage, and prefetch(). Pass null to stop reporting. */
void whiteout_casc_shim_setProgressCallback(void* self, whiteout_casc_progress_fn progress,
                                            void* user);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WHITEOUT_CASC_PROGRESS_H */
