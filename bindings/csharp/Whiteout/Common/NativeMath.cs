// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Runtime.InteropServices;

namespace Whiteout.Common;

/// <summary>
/// Hand-written P/Invoke entries for the shared-math allocators exposed by
/// <c>whiteout_c_common.cpp</c>. The codegen uses these when a scalar
/// field/method returns or accepts a math type by value — read the
/// returned 12 / 16 bytes via <c>Unsafe.Read</c>, then call the matching
/// delete to free the allocation.
/// </summary>
internal static partial class NativeMath
{
    [LibraryImport(Runtime.LibraryName)]
    internal static partial IntPtr whiteout_Vector2f_new();

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_Vector2f_delete(IntPtr self);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial IntPtr whiteout_Vector3f_new();

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_Vector3f_delete(IntPtr self);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial IntPtr whiteout_Vector4f_new();

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_Vector4f_delete(IntPtr self);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial IntPtr whiteout_Quaternion_new();

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_Quaternion_delete(IntPtr self);
}
