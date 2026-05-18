// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

package whiteout.interfaces.internal;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

/**
 * Lazy loader for the merged native library that hosts both the
 * Panama-callable C ABI symbols and the {@code Java_*} JNI upcall
 * symbols. The classes under {@code whiteout.interfaces.internal} all
 * funnel through {@link #ensureLoaded()} before their first native call.
 *
 * <p>Historically the JNI bridge and the C ABI shipped as two separate
 * DLLs ({@code whiteout_jni} + {@code whiteout_c}). They are now bundled
 * in {@code whiteout_c} alone; this loader honours both the canonical
 * {@code whiteout.native.path} system property and the legacy
 * {@code whiteout.jni.path} property for back-compat with existing
 * test harnesses.
 *
 * <p>Resolution order:
 * <ol>
 *   <li>System property {@code whiteout.native.path} (absolute path to .dll
 *       / .so / .dylib). Loaded via {@link System#load(String)}.</li>
 *   <li>Legacy {@code whiteout.jni.path} (same semantics).</li>
 *   <li>Standard library path search via
 *       {@link System#loadLibrary(String)} — name "whiteout_native".
 *       Honoured by {@code -Djava.library.path}, {@code LD_LIBRARY_PATH},
 *       and the PATH env var on Windows.</li>
 * </ol>
 */
public final class JniLoader {

    private static volatile boolean loaded = false;

    private JniLoader() {}

    public static synchronized void ensureLoaded() {
        if (loaded) return;
        String override = System.getProperty("whiteout.native.path");
        if (override == null || override.isEmpty()) {
            override = System.getProperty("whiteout.jni.path");
        }
        if (override != null && !override.isEmpty()) {
            Path p = Paths.get(override);
            if (!Files.isRegularFile(p)) {
                throw new RuntimeException(
                    "native-library path points at non-existent file: " + p);
            }
            System.load(p.toAbsolutePath().toString());
        } else {
            System.loadLibrary("whiteout_native");
        }
        loaded = true;
    }
}
