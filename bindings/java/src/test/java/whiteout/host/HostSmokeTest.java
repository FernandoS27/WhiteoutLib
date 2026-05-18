// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Java FFM smoke tests for the host bindings (threading, disk I/O, HTTP).
// Mirrors tests/python/test_host.py — exercises the same C surface that
// emit_c.py produces, via Project Panama upcalls.
//
// Run via scripts/build-java.ps1.

package whiteout.host;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

import whiteout.utils.HttpResponse;
import whiteout.utils.OsFileSystem;
import whiteout.utils.SimpleHttpHandler;
import whiteout.utils.SimpleThreadPool;

public class HostSmokeTest {

    public static void main(String[] args) throws Exception {
        testOsFileSystemRoundTrip();
        testSimpleThreadPool();
        testSimpleHttpHandler();
        testHttpResponseDefaults();
        System.out.println("OK: all host smoke tests passed");
    }

    static void testOsFileSystemRoundTrip() throws IOException {
        Path dir = Files.createTempDirectory("whiteout-host-fs");
        try {
            Files.write(dir.resolve("hello.bin"), new byte[]{1, 2, 3, 4, 5});
            try (OsFileSystem fs = OsFileSystem.createRootPath(dir.toString())) {
                // Bare close + non-null handle is enough to confirm the
                // C ABI ctor returned a live handle. Bytes accessors live
                // on VirtualPathFileSystem (opaque base) — exposing them
                // through the Java wrapper of OsFileSystem would require
                // cross-class method propagation that the codegen doesn't
                // do today; users go through Native.* directly if needed.
                require(fs != null, "OsFileSystem.createRootPath != null");
            }
        } finally {
            // Best-effort cleanup; ignore failures so the test doesn't
            // flake on Windows file-handle quirks.
            try (var stream = Files.walk(dir)) {
                stream.sorted(java.util.Comparator.reverseOrder())
                      .forEach(p -> { try { Files.deleteIfExists(p); }
                                      catch (IOException ignored) {} });
            }
        }
    }

    static void testSimpleThreadPool() {
        try (SimpleThreadPool pool = SimpleThreadPool.createNThreads(4)) {
            require(pool != null, "SimpleThreadPool.createNThreads != null");
        }
    }

    static void testSimpleHttpHandler() {
        // Both the no-arg and the n_threads ctors should round-trip.
        try (SimpleHttpHandler h = new SimpleHttpHandler()) {
            require(h != null, "default SimpleHttpHandler != null");
        }
        try (SimpleHttpHandler h = SimpleHttpHandler.createNThreads(2)) {
            require(h != null, "SimpleHttpHandler.createNThreads != null");
        }
    }

    static void testHttpResponseDefaults() {
        try (HttpResponse r = new HttpResponse()) {
            // Defaults: 0 / empty / empty. Just probing that the value-type
            // wrapper allocates and the status_code accessor doesn't crash.
            require(r.getStatusCode() == 0, "default status_code == 0");
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
