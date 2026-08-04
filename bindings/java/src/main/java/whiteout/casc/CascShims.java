// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN (not codegen). `Storage::openOnline` takes an options struct
// mixing an interface pointer with a std::function, and `Storage::readBatch`
// passes value objects in both directions — neither shape is something the
// codegen can express, so both cross through the shims in
// bindings/c/whiteout_casc_shims.cpp.

package whiteout.casc;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;

import whiteout.common.internal.NativeCommon;
import whiteout.host.HttpHandlers;
import whiteout.host.WorkerPools;
import whiteout.interfaces.HttpHandler;
import whiteout.interfaces.WorkerPool;

/** CASC entry points that need hand-written marshalling. */
public final class CascShims {

    private CascShims() {}

    private static final MethodHandle OPEN_ONLINE = NativeCommon.find(
        "whiteout_casc_shim_openOnline",
        FunctionDescriptor.of(ValueLayout.ADDRESS,
                              ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                              ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                              ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

    private static final MethodHandle READ_BATCH = NativeCommon.find(
        "whiteout_casc_shim_readBatch",
        FunctionDescriptor.of(ValueLayout.ADDRESS,
                              ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                              ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                              ValueLayout.JAVA_LONG));

    private static final MethodHandle READ_BATCH_COUNT = NativeCommon.find(
        "whiteout_casc_shim_readBatch_count",
        FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));

    private static final MethodHandle READ_BATCH_DATA_AT = NativeCommon.find(
        "whiteout_casc_shim_readBatch_data_at",
        FunctionDescriptor.of(NativeCommon.BYTES_LAYOUT,
                              ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));

    private static final MethodHandle READ_BATCH_SUCCESS_AT = NativeCommon.find(
        "whiteout_casc_shim_readBatch_success_at",
        FunctionDescriptor.of(ValueLayout.JAVA_INT,
                              ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));

    private static final MethodHandle READ_BATCH_ERROR_AT = NativeCommon.find(
        "whiteout_casc_shim_readBatch_error_at",
        FunctionDescriptor.of(NativeCommon.CSTRING_LAYOUT,
                              ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));

    private static final MethodHandle READ_BATCH_FREE = NativeCommon.find(
        "whiteout_casc_shim_readBatch_free",
        FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

    /**
     * One file to read in a batch. Use {@link #byPath} to read by CASC path,
     * or {@link #byFileId} to read by WoW-style FileDataId.
     */
    public record BatchReadRequest(String path, int fileDataId, FileIdHint hint) {

        /** Read by CASC path. */
        public static BatchReadRequest byPath(String path) {
            return new BatchReadRequest(Objects.requireNonNull(path, "path"),
                                        -1, FileIdHint.None);
        }

        /** Read by FileDataId. */
        public static BatchReadRequest byFileId(int fileDataId, FileIdHint hint) {
            return new BatchReadRequest(null, fileDataId,
                                        hint == null ? FileIdHint.None : hint);
        }
    }

    /**
     * Result of a single file in a batch read. {@code data} is null when the
     * read failed; {@code error} carries the diagnostic in that case.
     */
    public record BatchReadResult(byte[] data, boolean success, String error) {}

    /**
     * Open a CDN-backed (online) storage. The returned {@link Storage}
     * exposes the same read API as a local one.
     *
     * @param product    product code, e.g. "wow", "w3", "d3", "fenris"
     * @param region     region for version lookup; empty defaults to "us"
     * @param http       HTTP transport; required
     * @param buildKey   optional hex build-config key; null takes the latest
     * @param cacheDir   optional on-disk cache; null keeps everything in memory
     * @param localeMask locale filter, 0 accepts all
     * @param pool       optional worker pool for parallel I/O
     * @return the storage, or empty on failure
     */
    public static Optional<Storage> openOnline(String product, String region,
                                               HttpHandler http, String buildKey,
                                               String cacheDir, int localeMask,
                                               WorkerPool pool) {
        Objects.requireNonNull(http, "http");
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment productSeg = utf8(arena, product);
            MemorySegment regionSeg =
                utf8(arena, region == null || region.isEmpty() ? "us" : region);
            MemorySegment buildKeySeg = utf8(arena, buildKey == null ? "" : buildKey);
            MemorySegment cacheDirSeg = utf8(arena, cacheDir == null ? "" : cacheDir);

            long httpAddr = HttpHandlers.resolveNative(http, http);
            MemorySegment httpSeg = MemorySegment.ofAddress(httpAddr);
            long poolAddr = pool == null ? 0L : WorkerPools.resolveNative(pool, pool);
            MemorySegment poolSeg =
                poolAddr == 0L ? MemorySegment.NULL : MemorySegment.ofAddress(poolAddr);

            try {
                MemorySegment h = (MemorySegment) NativeCommon.invokeNative(
                    OPEN_ONLINE, productSeg, regionSeg, buildKeySeg,
                    httpSeg, cacheDirSeg, localeMask, poolSeg);
                if (h == null || h.equals(MemorySegment.NULL)) return Optional.empty();
                return Optional.of(new Storage(h, true));
            } finally {
                java.lang.ref.Reference.reachabilityFence(http);
                java.lang.ref.Reference.reachabilityFence(pool);
            }
        }
    }

    /**
     * Read every requested file in one native call. Results come back in
     * request order; an individual failure yields a result with
     * {@code success == false} and does not affect the others.
     *
     * <p>When the storage was opened with a worker pool, resolution / raw read
     * / BLTE decode overlap across files — considerably faster than reading
     * one file at a time.
     */
    public static List<BatchReadResult> readBatch(Storage storage,
                                                  List<BatchReadRequest> requests) {
        Objects.requireNonNull(storage, "storage");
        Objects.requireNonNull(requests, "requests");
        int n = requests.size();
        if (n == 0) return List.of();

        try (Arena arena = Arena.ofConfined()) {
            MemorySegment paths = arena.allocate(ValueLayout.ADDRESS, n);
            MemorySegment ids = arena.allocate(ValueLayout.JAVA_INT, n);
            MemorySegment hints = arena.allocate(ValueLayout.JAVA_INT, n);

            for (int i = 0; i < n; i++) {
                BatchReadRequest r = requests.get(i);
                if (r.path() != null) {
                    paths.setAtIndex(ValueLayout.ADDRESS, i, utf8(arena, r.path()));
                    ids.setAtIndex(ValueLayout.JAVA_INT, i, -1);
                    hints.setAtIndex(ValueLayout.JAVA_INT, i, 0);
                } else {
                    paths.setAtIndex(ValueLayout.ADDRESS, i, MemorySegment.NULL);
                    ids.setAtIndex(ValueLayout.JAVA_INT, i, r.fileDataId());
                    hints.setAtIndex(ValueLayout.JAVA_INT, i, r.hint().value);
                }
            }

            MemorySegment snap = (MemorySegment) NativeCommon.invokeNative(
                READ_BATCH, storage.handle, paths, ids, hints, (long) n);
            if (snap == null || snap.equals(MemorySegment.NULL)) return List.of();

            try {
                long count = (long) NativeCommon.invokeNative(READ_BATCH_COUNT, snap);
                var out = new ArrayList<BatchReadResult>((int) count);
                for (long i = 0; i < count; i++) {
                    boolean ok = ((int) NativeCommon.invokeNative(
                        READ_BATCH_SUCCESS_AT, snap, i)) != 0;
                    byte[] data = null;
                    if (ok) {
                        MemorySegment b = (MemorySegment) NativeCommon.invokeNative(
                            READ_BATCH_DATA_AT, arena, snap, i);
                        data = borrowedBytes(b);
                    }
                    MemorySegment e = (MemorySegment) NativeCommon.invokeNative(
                        READ_BATCH_ERROR_AT, arena, snap, i);
                    out.add(new BatchReadResult(data, ok, borrowedString(e)));
                }
                return out;
            } finally {
                NativeCommon.invokeNative(READ_BATCH_FREE, snap);
            }
        }
    }

    private static MemorySegment utf8(Arena arena, String s) {
        return s == null ? MemorySegment.NULL
                         : arena.allocateFrom(s, StandardCharsets.UTF_8);
    }

    // The snapshot accessors hand back borrowed views (`_owner` is null), so
    // these copy without freeing — the whole snapshot is released in one go.
    private static byte[] borrowedBytes(MemorySegment b) {
        if (b == null || b.equals(MemorySegment.NULL)) return new byte[0];
        MemorySegment data = b.get(ValueLayout.ADDRESS, 0);
        long size = b.get(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS.byteSize());
        if (data == null || data.equals(MemorySegment.NULL)) return new byte[0];
        return data.reinterpret(size).toArray(ValueLayout.JAVA_BYTE);
    }

    private static String borrowedString(MemorySegment s) {
        if (s == null || s.equals(MemorySegment.NULL)) return "";
        MemorySegment chars = s.get(ValueLayout.ADDRESS, 0);
        long len = s.get(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS.byteSize());
        if (chars == null || chars.equals(MemorySegment.NULL)) return "";
        return new String(chars.reinterpret(len).toArray(ValueLayout.JAVA_BYTE),
                          StandardCharsets.UTF_8);
    }
}
