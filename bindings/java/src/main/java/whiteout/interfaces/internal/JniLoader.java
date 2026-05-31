// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

package whiteout.interfaces.internal;

import whiteout.common.internal.NativeBinaryResolver;

/**
 * Lazy loader for the merged native library that hosts both the
 * Panama-callable C ABI symbols and the {@code Java_*} JNI upcall
 * symbols. Delegates to {@link NativeBinaryResolver} so the FFM path
 * (NativeCommon's static initialiser) and the JNI path land on the same
 * binary — important when the fat jar ships the binary as a classpath
 * resource and resolution extracts to a temp file.
 */
public final class JniLoader {
    private JniLoader() {}

    public static void ensureLoaded() {
        NativeBinaryResolver.load();
    }
}
