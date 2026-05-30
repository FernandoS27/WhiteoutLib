// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Runtime.InteropServices;

namespace Whiteout.Common;

/// <summary>
/// Hand-written P/Invoke declarations for symbols that live in
/// <c>whiteout_c_common.cpp</c> (free helpers shared by every module).
/// Generated module-specific stubs live under <c>Whiteout.Internal</c>.
/// </summary>
internal static partial class NativeCommon
{
    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_Bytes_free(NativeBytes buf);

    [LibraryImport(Runtime.LibraryName)]
    internal static partial void whiteout_CString_free(NativeCString str);
}
