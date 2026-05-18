// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Phase 3 JNI bridge smoke tests: VirtualPathFileSystem, WorkerPool,
// CascFileSystem. Each test passes a pure-Java implementation through
// the codegen-produced bridge factory and confirms the C++ side observes
// the Java overrides via the symmetric whiteout_jni_smoke_* invokers
// defined in bindings/java/jni/smoke_invokers.cpp.

package whiteout.interfaces;

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

import whiteout.interfaces.internal.CascFileSystemBridge;
import whiteout.interfaces.internal.VirtualPathFileSystemBridge;
import whiteout.interfaces.internal.WorkerPoolBridge;

public class BridgeSmokeTests {

    public static void main(String[] args) throws Throwable {
        testVirtualPathFileSystem();
        testWorkerPool();
        testWorkerPoolSubmitSimple();
        testWorkerPoolSubmitWithSemaphores();
        testWorkerPoolSubmitDependencyAware();
        testCascFileSystem();
        System.out.println("OK: all Phase 3 bridge smoke tests passed");
    }

    // ── VirtualPathFileSystem ─────────────────────────────────────────────

    static void testVirtualPathFileSystem() throws Throwable {
        final Map<String, byte[]> files = new HashMap<>();
        files.put("foo.bin", new byte[]{1, 2, 3, 4});

        VirtualPathFileSystem impl = new VirtualPathFileSystem() {
            @Override public byte[] readFile(String path) {
                byte[] data = files.get(path);
                return data == null ? new byte[0] : data;
            }
            @Override public boolean writeFile(String path, byte[] data) {
                files.put(path, data);
                return true;
            }
            @Override public boolean fileExists(String path) {
                return files.containsKey(path);
            }
        };

        Object owner = new Object();
        long h = VirtualPathFileSystemBridge.createPinned(impl, owner);
        // fileExists round-trip.
        require(NativeInvoker.vpfsFileExists(h, "foo.bin") == 1, "fileExists(foo) == true");
        require(NativeInvoker.vpfsFileExists(h, "missing") == 0, "fileExists(missing) == false");
        // readFile round-trip.
        byte[] read = NativeInvoker.vpfsReadFile(h, "foo.bin");
        require(java.util.Arrays.equals(read, new byte[]{1, 2, 3, 4}), "readFile round-tripped");
        // writeFile round-trip.
        require(NativeInvoker.vpfsWriteFile(h, "bar.bin", new byte[]{5, 6}) == 1, "writeFile == true");
        require(files.containsKey("bar.bin"), "Java side observed write");
        require(java.util.Arrays.equals(files.get("bar.bin"), new byte[]{5, 6}), "write payload");
    }

    // ── WorkerPool ────────────────────────────────────────────────────────

    static void testWorkerPool() throws Throwable {
        AtomicBoolean awaited = new AtomicBoolean(false);
        WorkerPool impl = new WorkerPool() {
            @Override public void submit(WorkerTask task) { /* unused here */ }
            @Override public void waitIdle()              { awaited.set(true); }
            @Override public long threadCount()           { return 7L; }
        };
        Object owner = new Object();
        long h = WorkerPoolBridge.createPinned(impl, owner);
        require(NativeInvoker.poolThreadCount(h) == 7, "threadCount == 7");
        NativeInvoker.poolWaitIdle(h);
        require(awaited.get(), "waitIdle observed on Java side");
    }

    // ── WorkerPool.submit — simple Runnable-style pool ────────────────────

    static void testWorkerPoolSubmitSimple() throws Throwable {
        // The Java pool just runs the WorkerTask (which is itself a
        // Runnable). Library code submits a task with no semaphores;
        // the task increments an atomic counter; the test polls.
        java.util.concurrent.ExecutorService exec =
            java.util.concurrent.Executors.newFixedThreadPool(2);
        WorkerPool pool = new WorkerPool() {
            @Override public void submit(WorkerTask task) { exec.submit(task); }
            @Override public void waitIdle()              { /* unused here */ }
            @Override public long threadCount()           { return 2; }
        };
        long flag = NativeInvoker.atomicNew();
        Object owner = new Object();
        try {
            long h = WorkerPoolBridge.createPinned(pool, owner);
            NativeInvoker.poolSubmit(h, flag, 0, 0, 0, 0);
            waitFor(() -> NativeInvoker.atomicGet(flag) == 1, 2000,
                "task ran within 2s");
        } finally {
            NativeInvoker.atomicFree(flag);
            exec.shutdown();
        }
    }

    // ── WorkerPool.submit — wait/signal semaphores honoured by run() ──────

    static void testWorkerPoolSubmitWithSemaphores() throws Throwable {
        // The task waits on `waitSem` reaching value 1 before running, and
        // signals `signalSem` to value 1 when done. The test signals
        // waitSem after a short delay, then verifies signalSem reaches 1.
        java.util.concurrent.ExecutorService exec =
            java.util.concurrent.Executors.newFixedThreadPool(2);
        WorkerPool pool = new WorkerPool() {
            @Override public void submit(WorkerTask task) { exec.submit(task); }
            @Override public void waitIdle()              {}
            @Override public long threadCount()           { return 2; }
        };
        long flag      = NativeInvoker.atomicNew();
        long waitSem   = NativeInvoker.semNew();
        long signalSem = NativeInvoker.semNew();
        Object owner = new Object();
        try {
            long h = WorkerPoolBridge.createPinned(pool, owner);
            NativeInvoker.poolSubmit(h, flag, waitSem, 1L, signalSem, 1L);
            // Task is queued + blocked on waitSem.await(1) — flag should
            // still be 0 a few ms in.
            Thread.sleep(50);
            require(NativeInvoker.atomicGet(flag) == 0,
                "task hasn't run before waitSem signalled");
            // Unblock the task.
            NativeInvoker.semSignal(waitSem, 1L);
            waitFor(() -> NativeInvoker.atomicGet(flag) == 1, 2000,
                "task ran after waitSem signalled");
            // signalSem should be signalled to 1 once the task completes.
            waitFor(() -> NativeInvoker.semValue(signalSem) >= 1, 2000,
                "signalSem reached 1");
        } finally {
            NativeInvoker.atomicFree(flag);
            NativeInvoker.semFree(waitSem);
            NativeInvoker.semFree(signalSem);
            exec.shutdown();
        }
    }

    // ── WorkerPool.submit — dependency-aware pool ─────────────────────────

    static void testWorkerPoolSubmitDependencyAware() throws Throwable {
        // The Java pool inspects task.waitSemaphore().value() before
        // queuing — defers tasks that aren't ready instead of blocking
        // an executor thread on await(). This is the whole reason we
        // exposed TimelineSemaphore as an opaque handle.
        java.util.concurrent.ExecutorService exec =
            java.util.concurrent.Executors.newFixedThreadPool(2);
        java.util.concurrent.ConcurrentLinkedDeque<WorkerTask> deferred =
            new java.util.concurrent.ConcurrentLinkedDeque<>();
        WorkerPool pool = new WorkerPool() {
            @Override public void submit(WorkerTask task) {
                if (task.waitSemaphore() != null
                        && task.waitSemaphore().value() < task.waitValue()) {
                    deferred.add(task);   // not ready — queue separately
                    return;
                }
                exec.submit(task);
            }
            @Override public void waitIdle()    {}
            @Override public long threadCount() { return 2; }
        };
        long flag    = NativeInvoker.atomicNew();
        long waitSem = NativeInvoker.semNew();
        Object owner = new Object();
        try {
            long h = WorkerPoolBridge.createPinned(pool, owner);
            NativeInvoker.poolSubmit(h, flag, waitSem, 1L, 0, 0);
            // The pool deferred the task because waitSem.value() == 0 < 1.
            Thread.sleep(50);
            require(deferred.size() == 1, "task was deferred (1 pending)");
            require(NativeInvoker.atomicGet(flag) == 0, "task hasn't run");
            // Signal the semaphore + drain the deferred queue ourselves.
            NativeInvoker.semSignal(waitSem, 1L);
            WorkerTask drained = deferred.pollFirst();
            require(drained != null, "drained the pending task");
            exec.submit(drained);
            waitFor(() -> NativeInvoker.atomicGet(flag) == 1, 2000,
                "task ran after manual drain");
        } finally {
            NativeInvoker.atomicFree(flag);
            NativeInvoker.semFree(waitSem);
            exec.shutdown();
        }
    }

    // Poll a condition with a deadline. Throws AssertionError on timeout.
    static void waitFor(java.util.function.BooleanSupplier predicate,
                        long maxMs, String message) throws InterruptedException {
        long deadline = System.currentTimeMillis() + maxMs;
        while (System.currentTimeMillis() < deadline) {
            if (predicate.getAsBoolean()) return;
            Thread.sleep(10);
        }
        throw new AssertionError("timeout waiting for: " + message);
    }

    // ── CascFileSystem ────────────────────────────────────────────────────

    static void testCascFileSystem() throws Throwable {
        final Map<Integer, byte[]> byId = new HashMap<>();
        byId.put(42, new byte[]{(byte) 0xCA, (byte) 0xFE});

        CascFileSystem impl = new CascFileSystem() {
            @Override public byte[] readFile(int fileId) {
                byte[] d = byId.get(fileId);
                return d == null ? new byte[0] : d;
            }
            @Override public Integer reserveFileId(String path) {
                if ("known".equals(path)) return 100;
                return null;
            }
            @Override public boolean writeFile(int fileId, byte[] data) {
                byId.put(fileId, data);
                return true;
            }
            @Override public boolean fileExists(int fileId) {
                return byId.containsKey(fileId);
            }
        };

        Object owner = new Object();
        long h = CascFileSystemBridge.createPinned(impl, owner);
        require(NativeInvoker.cascFileExists(h, 42) == 1, "fileExists(42)");
        require(NativeInvoker.cascFileExists(h, 99) == 0, "fileExists(99) == false");
        byte[] read = NativeInvoker.cascReadFile(h, 42);
        require(java.util.Arrays.equals(read, new byte[]{(byte) 0xCA, (byte) 0xFE}),
            "readFile round-tripped");
        require(NativeInvoker.cascReserveFileId(h, "known") == 100L, "reserveFileId(known) == 100");
        require(NativeInvoker.cascReserveFileId(h, "unknown") == -1L,
            "reserveFileId nullopt sentinel");
        require(NativeInvoker.cascWriteFile(h, 7, new byte[]{1, 2, 3}) == 1, "writeFile == true");
        require(byId.containsKey(7), "Java side observed write");
    }

    // ── Panama helpers for the smoke_invokers.cpp entry points ───────────

    private static final class NativeInvoker {
        private static final Arena ARENA = Arena.ofShared();
        private static final Linker LINKER = Linker.nativeLinker();

        private static MethodHandle link(String sym, FunctionDescriptor fd) {
            MemorySegment addr = SymbolLookup.loaderLookup().find(sym)
                .or(() -> LINKER.defaultLookup().find(sym))
                .orElseThrow(() -> new RuntimeException("symbol not found: " + sym));
            return LINKER.downcallHandle(addr, fd);
        }

        // Smoke invokers return SmokeBytes* — opaque pointer Java doesn't
        // need to layout-decode. Use the dedicated accessor entry points
        // to pull data / size out, then free with whiteout_jni_smoke_bytes_free.

        // ── VPFS handles ───────────────────────────────────────────────
        private static final MethodHandle VPFS_READ = link(
            "whiteout_jni_smoke_vpfs_readFile",
            FunctionDescriptor.of(ValueLayout.ADDRESS,
                ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));
        private static final MethodHandle VPFS_WRITE = link(
            "whiteout_jni_smoke_vpfs_writeFile",
            FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.JAVA_LONG, ValueLayout.ADDRESS,
                ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
        private static final MethodHandle VPFS_EXISTS = link(
            "whiteout_jni_smoke_vpfs_fileExists",
            FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));

        // ── Pool ───────────────────────────────────────────────────────
        private static final MethodHandle POOL_WAIT = link(
            "whiteout_jni_smoke_pool_waitIdle",
            FunctionDescriptor.ofVoid(ValueLayout.JAVA_LONG));
        private static final MethodHandle POOL_COUNT = link(
            "whiteout_jni_smoke_pool_threadCount",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));

        // ── CASC ───────────────────────────────────────────────────────
        private static final MethodHandle CASC_READ = link(
            "whiteout_jni_smoke_casc_readFile",
            FunctionDescriptor.of(ValueLayout.ADDRESS,
                ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT));
        private static final MethodHandle CASC_RESERVE = link(
            "whiteout_jni_smoke_casc_reserveFileId",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG,
                ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));
        private static final MethodHandle CASC_WRITE = link(
            "whiteout_jni_smoke_casc_writeFile",
            FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT,
                ValueLayout.ADDRESS, ValueLayout.JAVA_LONG));
        private static final MethodHandle CASC_EXISTS = link(
            "whiteout_jni_smoke_casc_fileExists",
            FunctionDescriptor.of(ValueLayout.JAVA_INT,
                ValueLayout.JAVA_LONG, ValueLayout.JAVA_INT));

        // ── SmokeBytes accessors / free ───────────────────────────────
        private static final MethodHandle BYTES_FREE = link(
            "whiteout_jni_smoke_bytes_free",
            FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
        private static final MethodHandle BYTES_DATA = link(
            "whiteout_jni_smoke_bytes_data",
            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        private static final MethodHandle BYTES_SIZE = link(
            "whiteout_jni_smoke_bytes_size",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));

        // ── Pool.submit + helpers ──────────────────────────────────────
        private static final MethodHandle POOL_SUBMIT = link(
            "whiteout_jni_smoke_pool_submit",
            FunctionDescriptor.ofVoid(
                ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG,
                ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG,
                ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));
        private static final MethodHandle ATOMIC_NEW = link(
            "whiteout_jni_smoke_atomic_new",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG));
        private static final MethodHandle ATOMIC_GET = link(
            "whiteout_jni_smoke_atomic_get",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_LONG));
        private static final MethodHandle ATOMIC_FREE = link(
            "whiteout_jni_smoke_atomic_free",
            FunctionDescriptor.ofVoid(ValueLayout.JAVA_LONG));
        private static final MethodHandle SEM_NEW = link(
            "whiteout_jni_smoke_sem_new",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG));
        private static final MethodHandle SEM_FREE = link(
            "whiteout_jni_smoke_sem_free",
            FunctionDescriptor.ofVoid(ValueLayout.JAVA_LONG));
        private static final MethodHandle SEM_SIGNAL = link(
            "whiteout_jni_smoke_sem_signal",
            FunctionDescriptor.ofVoid(ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));
        private static final MethodHandle SEM_VALUE = link(
            "whiteout_jni_smoke_sem_value",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));

        // These wrap MethodHandle.invoke (which throws Throwable) and
        // re-throw as RuntimeException so callers don't have to declare
        // `throws Throwable` everywhere — important for use inside
        // BooleanSupplier lambdas in waitFor().
        static void poolSubmit(long poolH, long flag,
                                long waitSem, long waitValue,
                                long signalSem, long signalValue) {
            try { POOL_SUBMIT.invoke(poolH, flag, waitSem, waitValue, signalSem, signalValue); }
            catch (Throwable t) { throw new RuntimeException(t); }
        }
        static long atomicNew() {
            try { return (long) ATOMIC_NEW.invoke(); }
            catch (Throwable t) { throw new RuntimeException(t); }
        }
        static int atomicGet(long h) {
            try { return (int) ATOMIC_GET.invoke(h); }
            catch (Throwable t) { throw new RuntimeException(t); }
        }
        static void atomicFree(long h) {
            try { ATOMIC_FREE.invoke(h); }
            catch (Throwable t) { throw new RuntimeException(t); }
        }
        static long semNew() {
            try { return (long) SEM_NEW.invoke(); }
            catch (Throwable t) { throw new RuntimeException(t); }
        }
        static void semFree(long h) {
            try { SEM_FREE.invoke(h); }
            catch (Throwable t) { throw new RuntimeException(t); }
        }
        static void semSignal(long h, long v) {
            try { SEM_SIGNAL.invoke(h, v); }
            catch (Throwable t) { throw new RuntimeException(t); }
        }
        static long semValue(long h) {
            try { return (long) SEM_VALUE.invoke(h); }
            catch (Throwable t) { throw new RuntimeException(t); }
        }

        static byte[] vpfsReadFile(long h, String path) throws Throwable {
            try (Arena a = Arena.ofConfined()) {
                MemorySegment p = a.allocateFrom(path);
                MemorySegment bytes = (MemorySegment) VPFS_READ.invoke(h, p);
                return unpackBytes(bytes);
            }
        }

        static int vpfsWriteFile(long h, String path, byte[] data) throws Throwable {
            try (Arena a = Arena.ofConfined()) {
                MemorySegment p = a.allocateFrom(path);
                MemorySegment d = a.allocate(Math.max(1, data.length));
                if (data.length > 0) d.asByteBuffer().put(data);
                return (int) VPFS_WRITE.invoke(h, p, d, (long) data.length);
            }
        }

        static int vpfsFileExists(long h, String path) throws Throwable {
            try (Arena a = Arena.ofConfined()) {
                MemorySegment p = a.allocateFrom(path);
                return (int) VPFS_EXISTS.invoke(h, p);
            }
        }

        static void poolWaitIdle(long h) throws Throwable { POOL_WAIT.invoke(h); }

        static long poolThreadCount(long h) throws Throwable {
            return (long) POOL_COUNT.invoke(h);
        }

        static byte[] cascReadFile(long h, int fileId) throws Throwable {
            MemorySegment bytes = (MemorySegment) CASC_READ.invoke(h, fileId);
            return unpackBytes(bytes);
        }

        static long cascReserveFileId(long h, String path) throws Throwable {
            try (Arena a = Arena.ofConfined()) {
                MemorySegment p = a.allocateFrom(path);
                return (long) CASC_RESERVE.invoke(h, p);
            }
        }

        static int cascWriteFile(long h, int fileId, byte[] data) throws Throwable {
            try (Arena a = Arena.ofConfined()) {
                MemorySegment d = a.allocate(Math.max(1, data.length));
                if (data.length > 0) d.asByteBuffer().put(data);
                return (int) CASC_WRITE.invoke(h, fileId, d, (long) data.length);
            }
        }

        static int cascFileExists(long h, int fileId) throws Throwable {
            return (int) CASC_EXISTS.invoke(h, fileId);
        }

        // SmokeBytes* — pull data/size via the dedicated accessors,
        // copy out, then free the C++ allocation.
        private static byte[] unpackBytes(MemorySegment smokeBytesPtr) throws Throwable {
            if (smokeBytesPtr.equals(MemorySegment.NULL)) return new byte[0];
            MemorySegment data = (MemorySegment) BYTES_DATA.invoke(smokeBytesPtr);
            long size = (long) BYTES_SIZE.invoke(smokeBytesPtr);
            byte[] out = new byte[(int) size];
            if (size > 0 && !data.equals(MemorySegment.NULL)) {
                data.reinterpret(size).asByteBuffer().get(out);
            }
            BYTES_FREE.invoke(smokeBytesPtr);
            return out;
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
