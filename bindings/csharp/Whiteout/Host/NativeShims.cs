// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Runtime.InteropServices;
using Whiteout.Common;

namespace Whiteout.Host.Internal;

/// <summary>
/// P/Invoke entries for the hand-written C# trampoline shims under
/// <c>bindings/c/whiteout_csharp_shims.cpp</c>.
/// </summary>
internal static partial class NativeShims
{
    // ── VirtualPathFileSystem ────────────────────────────────────────────

    [LibraryImport(Runtime.LibraryName)]
    internal static unsafe partial IntPtr whiteout_csharp_VirtualPathFileSystem_create(
        IntPtr userdata, void* fnTable);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_csharp_VirtualPathFileSystem_delete(IntPtr handle);

    // ── Smoke-test invokers — call virtual methods on a VFS handle ──────
    //
    // These are intended for tests, but they're also a clean way to verify
    // that any VFS handle (managed-subclass or native OsFileSystem) speaks
    // the same C++ virtual surface.

    [LibraryImport(Runtime.LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial NativeBytes whiteout_csharp_test_VirtualPathFileSystem_readFile(
        IntPtr handle, string path);

    [LibraryImport(Runtime.LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    [return: MarshalAs(UnmanagedType.I4)]
    internal static partial bool whiteout_csharp_test_VirtualPathFileSystem_fileExists(
        IntPtr handle, string path);

    [LibraryImport(Runtime.LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    [return: MarshalAs(UnmanagedType.I4)]
    internal static partial bool whiteout_csharp_test_VirtualPathFileSystem_writeFile(
        IntPtr handle, string path, ReadOnlySpan<byte> data, nuint size);

    // ── HttpHandler ──────────────────────────────────────────────────────

    [LibraryImport(Runtime.LibraryName)]
    internal static unsafe partial IntPtr whiteout_csharp_HttpHandler_create(
        IntPtr userdata, void* fnTable);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_csharp_HttpHandler_delete(IntPtr handle);

    [LibraryImport(Runtime.LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    internal static unsafe partial void whiteout_csharp_HttpResponseCallback_fire(
        IntPtr callbackHandle, int statusCode, byte* body, nuint bodyLen, string error);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_csharp_HttpResponseCallback_cancel(IntPtr callbackHandle);

    // Smoke-test invokers — fire managed methods through the C++ shim
    // and read back the response synchronously.

    [LibraryImport(Runtime.LibraryName)]
    internal static partial uint whiteout_csharp_test_HttpHandler_capabilities(IntPtr handle);

    [LibraryImport(Runtime.LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    internal static unsafe partial void whiteout_csharp_test_HttpHandler_getAsync(
        IntPtr handle, string url,
        int* outStatus, NativeBytes* outBody, NativeCString* outError);

    [LibraryImport(Runtime.LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    internal static unsafe partial void whiteout_csharp_test_HttpHandler_getRangeAsync(
        IntPtr handle, string url, ulong start, ulong end,
        int* outStatus, NativeBytes* outBody, NativeCString* outError);

    // ── WorkerPool ───────────────────────────────────────────────────────

    [LibraryImport(Runtime.LibraryName)]
    internal static unsafe partial IntPtr whiteout_csharp_WorkerPool_create(
        IntPtr userdata, void* fnTable);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_csharp_WorkerPool_delete(IntPtr handle);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_csharp_WorkerTaskFn_fire(IntPtr fnHandle);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_csharp_WorkerTaskFn_cancel(IntPtr fnHandle);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_csharp_TimelineSemaphore_await(IntPtr semHandle, ulong value);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_csharp_TimelineSemaphore_signal(IntPtr semHandle, ulong value);

    // Smoke-test invokers
    [LibraryImport(Runtime.LibraryName)]
    internal static partial nuint whiteout_csharp_test_WorkerPool_threadCount(IntPtr handle);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_csharp_test_WorkerPool_waitIdle(IntPtr handle);

    [LibraryImport(Runtime.LibraryName)]
    internal static unsafe partial void whiteout_csharp_test_WorkerPool_submitIncrementSentinel(
        IntPtr handle, int* outSentinel);
}
