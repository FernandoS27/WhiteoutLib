// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN (not codegen). P/Invoke for the CASC shims in
// bindings/c/whiteout_casc_shims.cpp — the two entry points whose C++
// signatures the codegen can't marshal (OnlineOpenOptions as a param
// struct; span<value_object> in / vector<value_object> out).

using System.Runtime.InteropServices;

namespace Whiteout.Casc.Internal;

internal static partial class NativeMethods
{
    // ── openOnline ─────────────────────────────────────────────────────────
    [LibraryImport(Runtime.LibraryName)]
    internal static partial IntPtr whiteout_casc_shim_openOnline(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string product,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string region,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string buildKey,
        IntPtr http,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string cacheDir,
        uint localeMask,
        IntPtr pool);

    // ── readBatch ──────────────────────────────────────────────────────────
    // `paths` holds UTF-8 C-string pointers; an entry may be IntPtr.Zero to
    // read by FileDataId instead. The caller owns and frees those pointers.
    [LibraryImport(Runtime.LibraryName)]
    internal static partial IntPtr whiteout_casc_shim_readBatch(
        IntPtr self,
        [In] IntPtr[] paths,
        [In] int[] fileDataIds,
        [In] int[] hints,
        nuint count);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial nuint whiteout_casc_shim_readBatch_count(IntPtr snapshot);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial Whiteout.Common.NativeBytes whiteout_casc_shim_readBatch_data_at(
        IntPtr snapshot, nuint index);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial int whiteout_casc_shim_readBatch_success_at(
        IntPtr snapshot, nuint index);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial Whiteout.Common.NativeCString whiteout_casc_shim_readBatch_error_at(
        IntPtr snapshot, nuint index);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_casc_shim_readBatch_free(IntPtr snapshot);
}
