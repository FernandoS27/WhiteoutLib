// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

package whiteout.interfaces;

import whiteout.interfaces.internal.JniLoader;

/**
 * Opaque Java handle wrapping a {@code whiteout::interfaces::TimelineSemaphore*}.
 * Instances are constructed by the JNI bridge (not by user code) and live as
 * long as the calling library owns the underlying C++ semaphore.
 *
 * <p><b>Lifetime contract.</b> Handles handed to a Java {@code WorkerPool} via
 * a {@link WorkerTask} are <em>borrowed</em> — the C++ caller owns the
 * underlying semaphore, and the Java handle must NOT be retained past the
 * task's {@code run()}. Keeping a reference longer dangles into freed memory
 * if the caller's {@code unique_ptr<TimelineSemaphore>} is dropped.
 *
 * <p>Most pool implementations don't need to interact with this class
 * directly — {@link WorkerTask#run()} honours the wait/signal contract via
 * its default implementation. Power users that want to defer tasks based on
 * semaphore state can inspect {@link #value()} before calling {@code run()}.
 *
 * <p>Named {@code await} rather than {@code wait} because {@code Object.wait}
 * is reserved and would shadow this method.
 */
public final class TimelineSemaphore {

    static {
        JniLoader.ensureLoaded();
    }

    /** Pointer to the C++ TimelineSemaphore. Borrowed — see class-level
     *  lifetime contract. */
    private final long handle;

    /** Package-private; produced by the JNI bridge when wrapping a C++ pointer. */
    TimelineSemaphore(long handle) {
        this.handle = handle;
    }

    /** Block until the timeline reaches at least {@code value}. */
    public void await(long value) { _await(handle, value); }

    /** Signal that the timeline reached at least {@code value}. */
    public void signal(long value) { _signal(handle, value); }

    /** Current completed value. */
    public long value() { return _value(handle); }

    /** Allocate the next timeline value (monotonically increasing). */
    public long next() { return _next(handle); }

    /** Raw native pointer — escape hatch for code that needs to pass the
     *  handle back into another native API. Do not free. */
    public long nativeHandle() { return handle; }

    private static native void _await(long handle, long value);
    private static native void _signal(long handle, long value);
    private static native long _value(long handle);
    private static native long _next(long handle);
}
