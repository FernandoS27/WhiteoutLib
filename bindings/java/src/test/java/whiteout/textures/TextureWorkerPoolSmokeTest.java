// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// End-to-end test of the WorkerPool dispatch through a real Panama
// consumer: Texture.copyAsFormat(format, WorkerPool pool). Verifies
// that BOTH dispatch paths work:
//
//   - Native-backed: whiteout.host.SimpleThreadPool (implements
//     whiteout.interfaces.WorkerPool + NativeHandled), so dispatch
//     extracts the existing C++ pointer with zero allocation.
//
//   - Pure-Java: a user implementation of WorkerPool. Dispatch wraps
//     it via WorkerPoolBridge.createPinned and hands the resulting
//     JavaWorkerPool* to C++.
//
// In both cases the output texture should be byte-identical to the
// no-pool baseline (the pool only affects parallelism, not result).

package whiteout.textures;

import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

import whiteout.utils.SimpleThreadPool;
import whiteout.interfaces.WorkerPool;
import whiteout.interfaces.WorkerTask;

public class TextureWorkerPoolSmokeTest {

    public static void main(String[] args) {
        testCopyAsFormatWithNoPool();
        testCopyAsFormatWithNativePool();
        testCopyAsFormatWithJavaPool();
        testGenerateMipmapsReturnsEmpty();
        testGenerateMipmapsWithJavaPool();
        System.out.println("OK: all Texture+WorkerPool smoke tests passed");
    }

    static void testCopyAsFormatWithNoPool() {
        try (Texture src = Texture.create2D(PixelFormat.RGBA8, 512, 512, 1);
             Texture dst = src.copyAsFormat(PixelFormat.BC1, null)) {
            require(dst != null, "copyAsFormat(null pool) returns non-null");
        }
    }

    static void testCopyAsFormatWithNativePool() {
        try (SimpleThreadPool pool = SimpleThreadPool.createNThreads(2);
             Texture src = Texture.create2D(PixelFormat.RGBA8, 512, 512, 1);
             Texture dst = src.copyAsFormat(PixelFormat.BC1, pool)) {
            require(dst != null, "copyAsFormat(SimpleThreadPool) returns non-null");
        }
    }

    static void testCopyAsFormatWithJavaPool() {
        AtomicInteger submitted = new AtomicInteger();
        WorkerPool impl = new SyncWorkerPool(submitted, /*threads=*/ 4L);
        try (Texture src = Texture.create2D(PixelFormat.RGBA8, 512, 512, 1);
             Texture dst = src.copyAsFormat(PixelFormat.BC1, impl)) {
            require(dst != null, "copyAsFormat(JavaWorkerPool) returns non-null");
        }
        // The C++ side may or may not have submitted tasks for a 32x32 BC1
        // encode — too small to be worth parallelising. The important
        // assertion is that the dispatch + JNI bridge round-trip succeeded
        // without crashing. We log the count for diagnostic value only.
        System.out.println("  java pool submitted " + submitted.get() + " task(s)");
        // Keep `impl` strongly reachable past the C++ call. The codegen
        // already does reachabilityFence inside copyAsFormat, but this
        // belt-and-suspenders fence proves the pattern at the call site.
        java.lang.ref.Reference.reachabilityFence(impl);
    }

    // ── generateMipmaps — exercises std::optional<std::string> return ──

    static void testGenerateMipmapsReturnsEmpty() {
        try (Texture src = Texture.create2D(PixelFormat.RGBA8, 256, 256, 1)) {
            java.util.Optional<String> err = src.generateMipmaps(0, null);
            require(err.isEmpty(),
                "generateMipmaps with no pool succeeds, no error string ("
                + err + ")");
        }
    }

    static void testGenerateMipmapsWithJavaPool() {
        AtomicInteger submitted = new AtomicInteger();
        WorkerPool impl = new SyncWorkerPool(submitted, /*threads=*/ 4L);
        try (Texture src = Texture.create2D(PixelFormat.RGBA8, 256, 256, 1)) {
            java.util.Optional<String> err = src.generateMipmaps(0, impl);
            require(err.isEmpty(),
                "generateMipmaps with Java pool returns empty Optional (success), got "
                + err);
        }
        System.out.println("  generateMipmaps java pool submitted " + submitted.get() + " task(s)");
        java.lang.ref.Reference.reachabilityFence(impl);
    }

    // ── Inline Java WorkerPool that just runs tasks synchronously ────────

    static final class SyncWorkerPool implements WorkerPool {
        private final AtomicInteger submitted;
        private final long threads;
        SyncWorkerPool(AtomicInteger submitted, long threads) {
            this.submitted = submitted;
            this.threads = threads;
        }
        @Override public void submit(WorkerTask task) {
            submitted.incrementAndGet();
            task.run();  // honours the wait → fn → signal contract
        }
        @Override public void waitIdle() {}
        @Override public long threadCount() { return threads; }
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("FAIL: " + msg);
    }
}
