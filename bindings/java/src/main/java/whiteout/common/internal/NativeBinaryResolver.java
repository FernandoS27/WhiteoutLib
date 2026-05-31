// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Resolve and load the whiteout_native shared library.
//
// Lookup order:
//   1. -Dwhiteout.native.path=...   absolute path to the binary
//   2. WHITEOUT_NATIVE_PATH env var same
//   3. META-INF/native/<rid>/<libname> on the classpath. The published
//      jar ships one binary per platform under that path; this method
//      extracts the right one to a temp file and System.load()s it.
//   4. System.loadLibrary("whiteout_native") — fallback for users who
//      put the binary on java.library.path themselves.
//
// Called by NativeCommon's static initialiser. Hand-written (NOT codegen
// output) so the loader logic stays stable across codegen regenerations.
package whiteout.common.internal;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

public final class NativeBinaryResolver {
    private NativeBinaryResolver() {}

    private static volatile boolean loaded = false;

    public static synchronized void load() {
        if (loaded) return;
        String override = System.getProperty("whiteout.native.path");
        if (override == null) override = System.getProperty("whiteout.jni.path");
        if (override == null) override = System.getenv("WHITEOUT_NATIVE_PATH");
        if (override != null && !override.isEmpty()) {
            System.load(override);
            loaded = true;
            return;
        }
        if (loadFromClasspath()) {
            loaded = true;
            return;
        }
        System.loadLibrary("whiteout_native");
        loaded = true;
    }

    private static boolean loadFromClasspath() {
        String rid = detectRid();
        String libName = libraryFileName();
        String resource = "/native/" + rid + "/" + libName;
        InputStream in = NativeBinaryResolver.class.getResourceAsStream(resource);
        if (in == null) return false;
        try (InputStream src = in) {
            Path tmp = Files.createTempFile("whiteout_native_", suffixFor(libName));
            tmp.toFile().deleteOnExit();
            Files.copy(src, tmp, StandardCopyOption.REPLACE_EXISTING);
            System.load(tmp.toAbsolutePath().toString());
            return true;
        } catch (IOException e) {
            throw new RuntimeException(
                "failed to extract whiteout_native from " + resource, e);
        }
    }

    private static String detectRid() {
        String os = System.getProperty("os.name", "").toLowerCase();
        String arch = System.getProperty("os.arch", "").toLowerCase();
        // Normalise to the same rid scheme used by the C# NuGet's
        // runtimes/ directory and AppVeyor's per-platform tags.
        String osTag;
        if (os.contains("win"))      osTag = "win";
        else if (os.contains("mac")) osTag = "osx";
        else                         osTag = "linux";
        String archTag;
        if (arch.equals("amd64") || arch.equals("x86_64")) archTag = "x64";
        else if (arch.equals("aarch64") || arch.equals("arm64")) archTag = "arm64";
        else                                                     archTag = arch;
        return osTag + "-" + archTag;
    }

    private static String libraryFileName() {
        String os = System.getProperty("os.name", "").toLowerCase();
        if (os.contains("win")) return "whiteout_native.dll";
        if (os.contains("mac")) return "libwhiteout_native.dylib";
        return "libwhiteout_native.so";
    }

    private static String suffixFor(String libName) {
        int dot = libName.lastIndexOf('.');
        return dot < 0 ? ".bin" : libName.substring(dot);
    }
}
