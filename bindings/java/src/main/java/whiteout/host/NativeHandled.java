// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

package whiteout.host;

import java.lang.foreign.MemorySegment;

/**
 * SPI marker for Java objects that wrap a C++ pointer directly. Implemented
 * by Panama-backed wrappers (e.g. {@code whiteout.host.SimpleHttpHandler}
 * adapters) so consumer bindings can recognise the native fast-path and
 * extract the underlying pointer without going through a JNI bridge.
 *
 * <p>Consumer bindings that take a {@code whiteout.interfaces.X}
 * abstraction should follow this dispatch pattern:
 * <pre>{@code
 * if (impl instanceof NativeHandled nh) {
 *     // Zero-cost: hand the C++ pointer straight to the native consumer.
 *     return nh.nativeHandle().address();
 * }
 * // Pure-Java impl — wrap via the interfaces.internal.*Bridge.
 * return XBridge.createPinned(impl, owner);
 * }</pre>
 *
 * <p>The returned {@link MemorySegment} is a borrowed view of the underlying
 * C++ allocation; its lifetime is governed by the wrapper that owns the
 * native object (typically a {@code try-with-resources} on the
 * {@code whiteout.host.*} wrapper).
 */
public interface NativeHandled {
    /** Borrowed view of the underlying C++ pointer. Never null. */
    MemorySegment nativeHandle();
}
