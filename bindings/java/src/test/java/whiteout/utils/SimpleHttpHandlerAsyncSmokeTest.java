// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Exercises the now-implemented SimpleHttpHandler.getAsync /
// getRangeAsync. Previously stubs throwing UnsupportedOperationException;
// now routed through JNI shims in native_http_async.cpp.
//
// Uses example.com (always-up, returns small known content) so the test
// doesn't depend on a local HTTP server. If the network is unavailable
// the test reports it via the callback's error string rather than
// hanging — that's still a pass for the dispatch wiring (the callback
// fired, which is what we're testing).

package whiteout.utils;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import whiteout.interfaces.HttpResponse;

public class SimpleHttpHandlerAsyncSmokeTest {

    public static void main(String[] args) throws Exception {
        testGetAsyncCallbackFires();
        testGetRangeAsyncCallbackFires();
        System.out.println("OK: all SimpleHttpHandler async smoke tests passed");
    }

    static void testGetAsyncCallbackFires() throws Exception {
        try (SimpleHttpHandler h = new SimpleHttpHandler()) {
            CountDownLatch done = new CountDownLatch(1);
            AtomicReference<HttpResponse> resp = new AtomicReference<>();
            h.getAsync("http://example.com/", r -> {
                resp.set(r);
                done.countDown();
            });
            require(done.await(15, TimeUnit.SECONDS),
                "getAsync callback fired within 15s");
            HttpResponse r = resp.get();
            require(r != null, "callback received a non-null HttpResponse");
            // Either real data (200) or a transport error — both prove
            // the dispatch wiring worked. We log for diagnostics.
            System.out.println("  status=" + r.statusCode()
                + " bodyLen=" + (r.body() == null ? 0 : r.body().length)
                + " error='" + r.error() + "'");
        }
    }

    static void testGetRangeAsyncCallbackFires() throws Exception {
        try (SimpleHttpHandler h = new SimpleHttpHandler()) {
            CountDownLatch done = new CountDownLatch(1);
            AtomicReference<HttpResponse> resp = new AtomicReference<>();
            h.getRangeAsync("http://example.com/", 0L, 31L, r -> {
                resp.set(r);
                done.countDown();
            });
            require(done.await(15, TimeUnit.SECONDS),
                "getRangeAsync callback fired within 15s");
            HttpResponse r = resp.get();
            require(r != null, "callback received a non-null HttpResponse");
            System.out.println("  range status=" + r.statusCode()
                + " bodyLen=" + (r.body() == null ? 0 : r.body().length)
                + " error='" + r.error() + "'");
        }
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("FAIL: " + msg);
    }
}
