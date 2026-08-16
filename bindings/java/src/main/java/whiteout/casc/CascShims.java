// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN (not codegen). `Storage::openOnline` takes an options struct
// mixing an interface pointer with a std::function, `Storage::readBatch`
// passes value objects in both directions, and progress reporting is a
// callback — none of those shapes is something the codegen can express, so
// they all cross through the shims in bindings/c/whiteout_casc_shims.cpp.

package whiteout.casc;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.WeakHashMap;

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

    private static final MethodHandle OPEN_WITH_PROGRESS = NativeCommon.find(
        "whiteout_casc_shim_openWithProgress",
        FunctionDescriptor.of(ValueLayout.ADDRESS,
                              ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                              ValueLayout.JAVA_INT, ValueLayout.JAVA_INT,
                              ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));

    private static final MethodHandle OPEN_ONLINE_WITH_PROGRESS = NativeCommon.find(
        "whiteout_casc_shim_openOnlineWithProgress",
        FunctionDescriptor.of(ValueLayout.ADDRESS,
                              ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                              ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                              ValueLayout.JAVA_INT, ValueLayout.JAVA_INT,
                              ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));

    private static final MethodHandle SET_PROGRESS_CALLBACK = NativeCommon.find(
        "whiteout_casc_shim_setProgressCallback",
        FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                                  ValueLayout.ADDRESS));

    private static final MethodHandle PROGRESS_STEP_NAME = NativeCommon.find(
        "whiteout_casc_shim_progressStepName",
        FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.JAVA_INT));

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

    // ── Progress reporting ────────────────────────────────────────────────
    //
    // The native side calls back through a cdecl function pointer. Rather than
    // route every handler through one static stub keyed by the `user` pointer,
    // each handler gets its own upcall stub with the handler bound into it —
    // `user` then carries nothing and is passed as NULL.

    /** Layout of {@code whiteout_casc_ProgressInfo}. */
    private static final MemoryLayout PROGRESS_INFO_LAYOUT = MemoryLayout.structLayout(
        ValueLayout.JAVA_INT.withName("size"),
        ValueLayout.JAVA_INT.withName("step"),
        ValueLayout.JAVA_INT.withName("state"),
        ValueLayout.JAVA_INT.withName("pad"),
        ValueLayout.ADDRESS.withName("object"),
        ValueLayout.JAVA_LONG.withName("current"),
        ValueLayout.JAVA_LONG.withName("total"),
        ValueLayout.JAVA_LONG.withName("bytesDone"),
        ValueLayout.JAVA_LONG.withName("bytesTotal"),
        ValueLayout.JAVA_INT.withName("stepIndex"),
        ValueLayout.JAVA_INT.withName("stepCount"),
        ValueLayout.JAVA_DOUBLE.withName("elapsedMs"),
        ValueLayout.JAVA_DOUBLE.withName("overallFraction")
    ).withName("whiteout_casc_ProgressInfo");

    private static final FunctionDescriptor PROGRESS_FN = FunctionDescriptor.of(
        ValueLayout.JAVA_INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS);

    private static final MethodHandle PROGRESS_TARGET;
    static {
        try {
            PROGRESS_TARGET = MethodHandles.lookup().findStatic(
                CascShims.class, "dispatchProgress",
                MethodType.methodType(int.class, CascProgressHandler.class, Throwable[].class,
                                      MemorySegment.class, MemorySegment.class));
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    /** Upcall stubs installed by {@link #setProgressCallback}, kept alive for
     *  as long as the storage they report on. */
    private static final Map<Storage, Object> INSTALLED =
        Collections.synchronizedMap(new WeakHashMap<>());

    /** A live upcall stub plus the slot its handler's exception lands in. */
    private record ProgressStub(MemorySegment address, Throwable[] fault) {}

    private static ProgressStub upcall(CascProgressHandler handler, Arena arena) {
        Throwable[] fault = new Throwable[1];
        MethodHandle bound = PROGRESS_TARGET.bindTo(handler).bindTo(fault);
        return new ProgressStub(NativeCommon.LINKER.upcallStub(bound, PROGRESS_FN, arena), fault);
    }

    private static int dispatchProgress(CascProgressHandler handler, Throwable[] fault,
                                        MemorySegment user, MemorySegment infoPtr) {
        if (infoPtr == null || infoPtr.equals(MemorySegment.NULL)) return 1;
        if (fault[0] != null) return 0; // already failing — stop asking
        try {
            MemorySegment info = infoPtr.reinterpret(PROGRESS_INFO_LAYOUT.byteSize());
            MemorySegment objectPtr = info.get(ValueLayout.ADDRESS, 16);
            String object = (objectPtr == null || objectPtr.equals(MemorySegment.NULL))
                ? ""
                : objectPtr.reinterpret(Long.MAX_VALUE).getString(0);

            var event = new CascProgressInfo(
                CascProgressStep.fromValue(info.get(ValueLayout.JAVA_INT, 4)),
                CascProgressState.fromValue(info.get(ValueLayout.JAVA_INT, 8)),
                object,
                info.get(ValueLayout.JAVA_LONG, 24),
                info.get(ValueLayout.JAVA_LONG, 32),
                info.get(ValueLayout.JAVA_LONG, 40),
                info.get(ValueLayout.JAVA_LONG, 48),
                info.get(ValueLayout.JAVA_INT, 56),
                info.get(ValueLayout.JAVA_INT, 60),
                info.get(ValueLayout.JAVA_DOUBLE, 64),
                info.get(ValueLayout.JAVA_DOUBLE, 72));

            return handler.onProgress(event) ? 1 : 0;
        } catch (Throwable t) {
            // Nothing may unwind through the native frame: remember it, cancel,
            // and rethrow once the native call has returned.
            fault[0] = t;
            return 0;
        }
    }

    private static void rethrow(Throwable[] fault) {
        Throwable t = fault[0];
        if (t == null) return;
        if (t instanceof RuntimeException e) throw e;
        if (t instanceof Error e) throw e;
        throw new RuntimeException("CASC progress handler threw", t);
    }

    /** English label for a progress step. */
    public static String progressStepName(CascProgressStep step) {
        Objects.requireNonNull(step, "step");
        MemorySegment p = (MemorySegment) NativeCommon.invokeNative(
            PROGRESS_STEP_NAME, step.value);
        if (p == null || p.equals(MemorySegment.NULL)) return step.name();
        return p.reinterpret(Long.MAX_VALUE).getString(0);
    }

    /**
     * Open a local storage, reporting progress as it goes.
     *
     * <p>The handler is called for every event until it returns false, which
     * cancels the open — the result is then empty and
     * {@code Storage.lastError()} reports cancellation.
     *
     * @param path       game directory, or its Data subdirectory
     * @param progress   progress handler; null opens without reporting
     * @param product    optional product code selecting a build from a
     *                   multi-product {@code .build.info}, e.g. "w3" vs "w3t"
     * @param localeMask locale filter, 0 accepts all
     * @param flags      {@code StorageFeatureFlags} bitmask; 0 loads eagerly
     * @param pool       optional worker pool for parallel I/O
     * @return the storage, or empty on failure or cancellation
     */
    public static Optional<Storage> openWithProgress(String path, CascProgressHandler progress,
                                                     String product, int localeMask, int flags,
                                                     WorkerPool pool) {
        Objects.requireNonNull(path, "path");
        try (Arena arena = Arena.ofConfined()) {
            ProgressStub stub = progress == null ? null : upcall(progress, arena);
            long poolAddr = pool == null ? 0L : WorkerPools.resolveNative(pool, pool);

            try {
                MemorySegment h = (MemorySegment) NativeCommon.invokeNative(
                    OPEN_WITH_PROGRESS,
                    utf8(arena, path),
                    utf8(arena, product == null ? "" : product),
                    localeMask, flags,
                    stub == null ? MemorySegment.NULL : stub.address(),
                    MemorySegment.NULL,
                    poolAddr == 0L ? MemorySegment.NULL : MemorySegment.ofAddress(poolAddr));
                if (stub != null) rethrow(stub.fault());
                if (h == null || h.equals(MemorySegment.NULL)) return Optional.empty();
                return Optional.of(new Storage(h, true));
            } finally {
                java.lang.ref.Reference.reachabilityFence(pool);
            }
        }
    }

    /**
     * Open a CDN-backed storage, reporting progress as it goes. Same reporting
     * and cancellation rules as {@link #openWithProgress}.
     *
     * @param flags {@code StorageFeatureFlags} bitmask; 0 keeps the online
     *              default (fully lazy)
     */
    public static Optional<Storage> openOnlineWithProgress(String product, String region,
                                                           HttpHandler http,
                                                           CascProgressHandler progress,
                                                           String buildKey, String cacheDir,
                                                           int localeMask, int flags,
                                                           WorkerPool pool) {
        Objects.requireNonNull(http, "http");
        try (Arena arena = Arena.ofConfined()) {
            ProgressStub stub = progress == null ? null : upcall(progress, arena);
            long httpAddr = HttpHandlers.resolveNative(http, http);
            long poolAddr = pool == null ? 0L : WorkerPools.resolveNative(pool, pool);

            try {
                MemorySegment h = (MemorySegment) NativeCommon.invokeNative(
                    OPEN_ONLINE_WITH_PROGRESS,
                    utf8(arena, product),
                    utf8(arena, region == null || region.isEmpty() ? "us" : region),
                    utf8(arena, buildKey == null ? "" : buildKey),
                    MemorySegment.ofAddress(httpAddr),
                    utf8(arena, cacheDir == null ? "" : cacheDir),
                    localeMask, flags,
                    stub == null ? MemorySegment.NULL : stub.address(),
                    MemorySegment.NULL,
                    poolAddr == 0L ? MemorySegment.NULL : MemorySegment.ofAddress(poolAddr));
                if (stub != null) rethrow(stub.fault());
                if (h == null || h.equals(MemorySegment.NULL)) return Optional.empty();
                return Optional.of(new Storage(h, true));
            } finally {
                java.lang.ref.Reference.reachabilityFence(http);
                java.lang.ref.Reference.reachabilityFence(pool);
            }
        }
    }

    /**
     * Report progress for the work that happens after open: the deferred load a
     * {@code LoadOnDemand} storage does on first access, and {@code prefetch()}.
     * Each of those reports as its own operation, ending with a Ready event.
     *
     * <p>The stub lives in an automatic arena kept reachable from the storage,
     * so it stays valid for as long as the storage does. Pass null to stop
     * reporting. An exception thrown by the handler cancels the operation in
     * progress and is not rethrown — there is no call to rethrow it on.
     */
    public static void setProgressCallback(Storage storage, CascProgressHandler progress) {
        Objects.requireNonNull(storage, "storage");
        if (progress == null) {
            NativeCommon.invokeNative(SET_PROGRESS_CALLBACK, storage.handle,
                                      MemorySegment.NULL, MemorySegment.NULL);
            INSTALLED.remove(storage);
            return;
        }
        // Auto arena: the stub is freed once nothing references it, which is
        // after the map entry goes — never while native code still holds it.
        ProgressStub stub = upcall(progress, Arena.ofAuto());
        INSTALLED.put(storage, stub);
        NativeCommon.invokeNative(SET_PROGRESS_CALLBACK, storage.handle,
                                  stub.address(), MemorySegment.NULL);
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
