// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// JNI smoke test for the codegen-produced HttpHandler bridge.
//
// HttpHandler.getAsync / getRangeAsync take `Consumer<HttpResponse>`
// callbacks. The Java impl invokes `complete.accept(response)` to fire
// the C++ std::function; the bridge unwinds back into the C++ callback
// the library originally passed in.
//
// The C++ smoke invokers in smoke_invokers.cpp synthesise a getAsync
// call with a capturing lambda and let the std::function fire from the
// Java side, then read the captured response back into Java. The test
// drives both synchronous and deferred completion patterns.

package whiteout.interfaces;

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

import whiteout.interfaces.internal.HttpHandlerBridge;

public class HttpHandlerBridgeSmokeTest {

    public static void main(String[] args) throws Throwable {
        testGetAsyncSynchronousComplete();
        testGetAsyncDeferredComplete();
        testCapabilitiesDefault();
        testCapabilitiesOverride();
        testGetRangeAsyncRoundTrip();
        testCancelReleasesCallback();
        testNullImplThrows();
        System.out.println("OK: all HttpHandler bridge smoke tests passed");
    }

    static void testGetAsyncSynchronousComplete() {
        final byte[] payload = "hello-jni".getBytes();
        final String[] seenUrl = new String[1];
        HttpHandler h = new HttpHandler() {
            @Override
            public void getAsync(String url, Consumer<HttpResponse> complete) {
                seenUrl[0] = url;
                // Fire on the calling thread — simplest pattern.
                complete.accept(new HttpResponse(200, payload, ""));
            }
            @Override
            public void getRangeAsync(String u, long s, long e, Consumer<HttpResponse> complete) {
                throw new AssertionError("getRangeAsync called unexpectedly");
            }
        };
        Object owner = new Object();   // pin lifetime of the bridge
        long ptr = HttpHandlerBridge.createPinned(h, owner);
        HttpResponse resp = SmokeInvoker.invokeGetAsync(ptr, "https://example/foo");
        require("https://example/foo".equals(seenUrl[0]),
            "Java handler observed the URL");
        require(resp.statusCode() == 200, "status_code == 200");
        require(java.util.Arrays.equals(resp.body(), payload), "body round-tripped");
        require(resp.error().isEmpty(), "no error");
        // owner is reachable until method exit — Cleaner won't fire prematurely
    }

    static void testGetAsyncDeferredComplete() throws Throwable {
        // Schedule completion onto another thread to validate the bridge
        // doesn't require synchronous firing — this is the whole reason
        // we switched away from the sync_call sugar.
        ScheduledExecutorService exec = Executors.newSingleThreadScheduledExecutor();
        final byte[] payload = "deferred".getBytes();
        HttpHandler h = new HttpHandler() {
            @Override
            public void getAsync(String url, Consumer<HttpResponse> complete) {
                exec.schedule(
                    () -> complete.accept(new HttpResponse(200, payload, "")),
                    20, TimeUnit.MILLISECONDS);
            }
            @Override
            public void getRangeAsync(String u, long s, long e, Consumer<HttpResponse> complete) {
                throw new AssertionError();
            }
        };
        Object owner = new Object();
        long ptr = HttpHandlerBridge.createPinned(h, owner);
        try {
            // The C++ smoke invoker captures the response into its `out`
            // local, but only when the callback fires. We need to wait
            // until the deferred completion runs.
            HttpResponse resp = SmokeInvoker.invokeGetAsyncWaitMs(
                ptr, "https://example/foo", 1000);
            require(resp.statusCode() == 200, "deferred status_code == 200");
            require(java.util.Arrays.equals(resp.body(), payload), "deferred body");
        } finally {
            exec.shutdown();
        }
    }

    static void testCapabilitiesDefault() {
        HttpHandler h = new HttpHandler() {
            @Override
            public void getAsync(String url, Consumer<HttpResponse> complete) {
                complete.accept(new HttpResponse(200, new byte[0], ""));
            }
            @Override
            public void getRangeAsync(String u, long s, long e, Consumer<HttpResponse> complete) {
                complete.accept(new HttpResponse(200, new byte[0], ""));
            }
        };
        Object owner = new Object();
        long ptr = HttpHandlerBridge.createPinned(h, owner);
        int caps = SmokeInvoker.invokeCapabilities(ptr);
        require(caps == 0, "default capabilities == 0");
    }

    static void testCapabilitiesOverride() {
        HttpHandler h = new HttpHandler() {
            @Override public int capabilities() { return 1; }
            @Override
            public void getAsync(String url, Consumer<HttpResponse> complete) {
                complete.accept(new HttpResponse(200, new byte[0], ""));
            }
            @Override
            public void getRangeAsync(String u, long s, long e, Consumer<HttpResponse> complete) {
                complete.accept(new HttpResponse(200, new byte[0], ""));
            }
        };
        Object owner = new Object();
        long ptr = HttpHandlerBridge.createPinned(h, owner);
        int caps = SmokeInvoker.invokeCapabilities(ptr);
        require(caps == 1, "capabilities override flows back to C++");
    }

    static void testGetRangeAsyncRoundTrip() {
        final long[] seenStart = new long[1];
        final long[] seenEnd   = new long[1];
        HttpHandler h = new HttpHandler() {
            @Override
            public void getAsync(String url, Consumer<HttpResponse> complete) {
                throw new AssertionError("getAsync called unexpectedly");
            }
            @Override
            public void getRangeAsync(String url, long s, long e, Consumer<HttpResponse> complete) {
                seenStart[0] = s;
                seenEnd[0]   = e;
                complete.accept(new HttpResponse(206,
                    ("range " + s + "-" + e).getBytes(), ""));
            }
        };
        Object owner = new Object();
        long ptr = HttpHandlerBridge.createPinned(h, owner);
        HttpResponse resp = SmokeInvoker.invokeGetRangeAsync(
            ptr, "https://example/foo", 100, 200);
        require(seenStart[0] == 100, "start propagated");
        require(seenEnd[0]   == 200, "end propagated");
        require(resp.statusCode() == 206, "206 returned");
        require(new String(resp.body()).equals("range 100-200"), "range body round-tripped");
    }

    static void testCancelReleasesCallback() {
        // The Java side never fires accept(); it calls cancel() instead.
        // The C++ std::function should be freed; the invoking lambda's
        // captured `out` stays at its default-constructed value.
        HttpHandler h = new HttpHandler() {
            @Override
            public void getAsync(String url, Consumer<HttpResponse> complete) {
                ((HttpResponseConsumer) complete).cancel();
            }
            @Override
            public void getRangeAsync(String u, long s, long e, Consumer<HttpResponse> complete) {
                ((HttpResponseConsumer) complete).cancel();
            }
        };
        Object owner = new Object();
        long ptr = HttpHandlerBridge.createPinned(h, owner);
        HttpResponse resp = SmokeInvoker.invokeGetAsync(ptr, "https://example/foo");
        // Default-constructed HttpResponse: status=0, empty body, empty error.
        require(resp.statusCode() == 0, "cancel left status at 0");
        require(resp.body().length == 0, "cancel left body empty");
    }

    static void testNullImplThrows() {
        Object owner = new Object();
        try {
            HttpHandlerBridge.createPinned(null, owner);
            throw new AssertionError("expected NullPointerException");
        } catch (NullPointerException expected) {
            // good
        }
    }

    // ── Helper: call the bridged HttpHandler* directly via Panama. ──────
    private static final class SmokeInvoker {
        private static final Arena ARENA = Arena.ofShared();
        private static final Linker LINKER = Linker.nativeLinker();

        private static MethodHandle link(String sym, FunctionDescriptor fd) {
            MemorySegment addr = SymbolLookup.loaderLookup().find(sym)
                .or(() -> LINKER.defaultLookup().find(sym))
                .orElseThrow(() -> new RuntimeException("symbol not found: " + sym));
            return LINKER.downcallHandle(addr, fd);
        }

        private static final MethodHandle INVOKE_GET_ASYNC = link(
            "whiteout_jni_smoke_invoke_getAsync",
            FunctionDescriptor.of(ValueLayout.ADDRESS,
                ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));
        private static final MethodHandle INVOKE_GET_ASYNC_WAIT = link(
            "whiteout_jni_smoke_invoke_getAsync_wait_ms",
            FunctionDescriptor.of(ValueLayout.ADDRESS,
                ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
        private static final MethodHandle INVOKE_GET_RANGE = link(
            "whiteout_jni_smoke_invoke_getRangeAsync",
            FunctionDescriptor.of(ValueLayout.ADDRESS,
                ValueLayout.JAVA_LONG, ValueLayout.ADDRESS,
                ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));
        private static final MethodHandle INVOKE_CAPS = link(
            "whiteout_jni_smoke_invoke_capabilities",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_LONG));
        private static final MethodHandle FREE_RESPONSE = link(
            "whiteout_jni_smoke_response_free",
            FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
        private static final MethodHandle RESPONSE_STATUS = link(
            "whiteout_jni_smoke_response_status",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
        private static final MethodHandle RESPONSE_BODY_PTR = link(
            "whiteout_jni_smoke_response_body_ptr",
            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        private static final MethodHandle RESPONSE_BODY_LEN = link(
            "whiteout_jni_smoke_response_body_len",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS));
        private static final MethodHandle RESPONSE_ERROR = link(
            "whiteout_jni_smoke_response_error",
            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS));

        static HttpResponse invokeGetAsync(long handle, String url) {
            try {
                MemorySegment u = ARENA.allocateFrom(url);
                MemorySegment resp = (MemorySegment) INVOKE_GET_ASYNC.invoke(handle, u);
                return unpack(resp);
            } catch (Throwable t) { throw new RuntimeException(t); }
        }

        static HttpResponse invokeGetAsyncWaitMs(long handle, String url, int waitMs) {
            try {
                MemorySegment u = ARENA.allocateFrom(url);
                MemorySegment resp = (MemorySegment) INVOKE_GET_ASYNC_WAIT.invoke(handle, u, waitMs);
                return unpack(resp);
            } catch (Throwable t) { throw new RuntimeException(t); }
        }

        static HttpResponse invokeGetRangeAsync(long handle, String url, long s, long e) {
            try {
                MemorySegment u = ARENA.allocateFrom(url);
                MemorySegment resp = (MemorySegment) INVOKE_GET_RANGE.invoke(handle, u, s, e);
                return unpack(resp);
            } catch (Throwable t) { throw new RuntimeException(t); }
        }

        static int invokeCapabilities(long handle) {
            try {
                return (int) INVOKE_CAPS.invoke(handle);
            } catch (Throwable t) { throw new RuntimeException(t); }
        }

        static HttpResponse unpack(MemorySegment respPtr) throws Throwable {
            int status = (int) RESPONSE_STATUS.invoke(respPtr);
            MemorySegment bodyPtr = (MemorySegment) RESPONSE_BODY_PTR.invoke(respPtr);
            long bodyLen = (long) RESPONSE_BODY_LEN.invoke(respPtr);
            byte[] body = new byte[(int) bodyLen];
            if (bodyLen > 0) {
                bodyPtr.reinterpret(bodyLen).asByteBuffer().get(body);
            }
            MemorySegment errPtr = (MemorySegment) RESPONSE_ERROR.invoke(respPtr);
            String err = errPtr.equals(MemorySegment.NULL) ? ""
                : errPtr.reinterpret(Long.MAX_VALUE).getString(0L);
            FREE_RESPONSE.invoke(respPtr);
            return new HttpResponse(status, body, err);
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
