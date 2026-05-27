// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// End-to-end demo of the polymorphic-dispatch design for consumer
// bindings that take a `whiteout::interfaces::HttpHandler*`:
//
//   - A whiteout.host.SimpleHttpHandler (Panama-wrapped C++ impl) is
//     passed directly — it implements whiteout.interfaces.HttpHandler +
//     NativeHandled, so HttpHandlers.resolveNative() returns the
//     existing C++ pointer with zero allocation.
//
//   - A pure-Java HttpHandler is wrapped via the JNI bridge and produces
//     a freshly-allocated JavaHttpHandler* pointer.
//
// Both pointers are then handed to `whiteout_jni_smoke_invoke_capabilities`
// — a real C++ function in whiteout_jni.dll that takes a HttpHandler* and
// calls `capabilities()` on it. The returned value proves the consumer
// reached the right implementation in both cases.

package whiteout.host;

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.util.function.Consumer;

import whiteout.interfaces.HttpHandler;
import whiteout.interfaces.HttpResponse;
import whiteout.host.OsFileSystem;
import whiteout.utils.SimpleHttpHandler;

public class NativeDispatchSmokeTest {

    public static void main(String[] args) {
        testNativeFastPath();
        testPureJavaBridgePath();
        testHostWrappersImplementInterface();
        testEndToEndConsumerNative();
        testEndToEndConsumerPureJava();
        testVpfsDispatchNative();
        testVpfsDispatchPureJava();
        System.out.println("OK: all NativeDispatch smoke tests passed");
    }

    // ── Native fast-path ──────────────────────────────────────────────────

    static void testNativeFastPath() {
        try (SimpleHttpHandler nat = new SimpleHttpHandler()) {
            long expected = nat.nativeHandle().address();
            long resolved = HttpHandlers.resolveNative(nat, /*owner=*/ nat);
            require(resolved == expected,
                "fast-path: resolveNative returns SimpleHttpHandler.handle directly");
            int caps = nat.capabilities();
            require(caps >= 0, "capabilities() forwards through Panama");
        }
    }

    // ── Pure-Java bridge path ─────────────────────────────────────────────

    static void testPureJavaBridgePath() {
        HttpHandler impl = pureJavaImpl(42);
        Object owner = new Object();
        long handle = HttpHandlers.resolveNative(impl, owner);
        require(handle != 0L, "bridge path: resolveNative returns a non-null handle");
        java.lang.ref.Reference.reachabilityFence(owner);
    }

    // ── Host wrappers implement the interface directly ────────────────────

    static void testHostWrappersImplementInterface() {
        try (SimpleHttpHandler nat = new SimpleHttpHandler()) {
            HttpHandler asIface = nat;            // straight assignment, no adapter
            require(asIface instanceof NativeHandled,
                "Panama wrapper implements NativeHandled");
            require(((NativeHandled) asIface).nativeHandle().address()
                    == nat.nativeHandle().address(),
                "nativeHandle() returns the underlying Panama handle");
            // getAsync used to throw UnsupportedOperationException — it
            // now routes through the JNI shim in native_http_async.cpp
            // and actually fires the Consumer. Cover that elsewhere
            // (SimpleHttpHandlerAsyncSmokeTest); here just make sure the
            // call doesn't throw synchronously.
            try {
                asIface.getAsync("http://example/", r -> {});
            } catch (Exception ex) {
                throw new AssertionError("getAsync no longer stubs out: " + ex);
            }
        }
    }

    // ── End-to-end: native impl consumed by C++ ───────────────────────────

    static void testEndToEndConsumerNative() {
        try (SimpleHttpHandler nat = new SimpleHttpHandler()) {
            long ptr = HttpHandlers.resolveNative(nat, nat);
            int viaPanama = nat.capabilities();
            int viaConsumer = NativeConsumer.invokeCapabilities(ptr);
            require(viaPanama == viaConsumer,
                "native path: C++ consumer sees the same caps as Panama (" +
                viaPanama + " vs " + viaConsumer + ")");
        }
    }

    // ── End-to-end: Java impl consumed by C++ ─────────────────────────────

    static void testEndToEndConsumerPureJava() {
        HttpHandler impl = pureJavaImpl(0x1337);
        Object owner = new Object();
        long ptr = HttpHandlers.resolveNative(impl, owner);
        try {
            int observed = NativeConsumer.invokeCapabilities(ptr);
            require(observed == 0x1337,
                "bridge path: C++ consumer invoked Java capabilities() and got 0x1337, got " +
                Integer.toHexString(observed));
        } finally {
            java.lang.ref.Reference.reachabilityFence(owner);
        }
    }

    // ── VirtualPathFileSystem dispatch (proves the design is per-interface) ─

    static void testVpfsDispatchNative() {
        try (OsFileSystem fs = OsFileSystem.createRootPath(".")) {
            long expected = fs.nativeHandle().address();
            long resolved = VirtualPathFileSystems.resolveNative(fs, fs);
            require(resolved == expected,
                "VPFS native fast-path: resolveNative returns OsFileSystem.handle");
        }
    }

    static void testVpfsDispatchPureJava() {
        whiteout.interfaces.VirtualPathFileSystem impl =
            new whiteout.interfaces.VirtualPathFileSystem() {
                @Override public byte[] readFile(String path) { return new byte[]{1, 2, 3}; }
                @Override public boolean writeFile(String path, byte[] data) { return true; }
                @Override public boolean fileExists(String path) { return path.equals("known"); }
            };
        Object owner = new Object();
        long handle = VirtualPathFileSystems.resolveNative(impl, owner);
        require(handle != 0L, "VPFS bridge path: resolveNative returns a non-null handle");
        java.lang.ref.Reference.reachabilityFence(owner);
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    private static HttpHandler pureJavaImpl(int caps) {
        return new HttpHandler() {
            @Override public int capabilities() { return caps; }
            @Override public void getAsync(String url, Consumer<HttpResponse> cb) {
                throw new AssertionError("not invoked");
            }
            @Override public void getRangeAsync(String url, long s, long e,
                                                Consumer<HttpResponse> cb) {
                throw new AssertionError("not invoked");
            }
        };
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("FAIL: " + msg);
    }

    // ── FFM linkage to the C++ smoke consumer (lives in whiteout_c.dll now
    //    that the JNI bridge sources are bundled into the C ABI library)

    private static final class NativeConsumer {
        private static final Linker LINKER = Linker.nativeLinker();
        private static final SymbolLookup LOOKUP;
        static {
            // Ensure the merged DLL (Panama C ABI + JNI bridge sources)
            // is loaded. JniLoader does the same `System.loadLibrary`
            // (no-op if NativeCommon already loaded it for Panama use).
            whiteout.interfaces.internal.JniLoader.ensureLoaded();
            LOOKUP = SymbolLookup.loaderLookup();
        }
        private static final MethodHandle INVOKE_CAPS = LINKER.downcallHandle(
            LOOKUP.find("whiteout_jni_smoke_invoke_capabilities").orElseThrow(
                () -> new RuntimeException("symbol not found: whiteout_jni_smoke_invoke_capabilities")),
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_LONG));

        static int invokeCapabilities(long handle) {
            try { return (int) INVOKE_CAPS.invoke(handle); }
            catch (Throwable t) { throw new RuntimeException(t); }
        }
    }
}
