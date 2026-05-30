// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Runtime.InteropServices;

namespace Whiteout;

/// <summary>
/// Runtime configuration for the Whiteout native library.
/// </summary>
/// <remarks>
/// In packaged consumers the native library is resolved automatically from
/// the NuGet <c>runtimes/&lt;rid&gt;/native/</c> layout — no configuration
/// is required. For repo-local development, set
/// <see cref="NativeLibraryPath"/> before any other Whiteout type is used,
/// or set the <c>WHITEOUT_NATIVE_PATH</c> environment variable to point at
/// the absolute path of <c>whiteout_native.dll</c> / <c>.so</c> /
/// <c>.dylib</c>.
/// </remarks>
public static class Runtime
{
    /// <summary>
    /// Absolute path to <c>whiteout_native</c>. Highest-priority override —
    /// if set, used in preference to <c>WHITEOUT_NATIVE_PATH</c> and the
    /// OS search path. Must be set before the first call into Whiteout.
    /// </summary>
    public static string? NativeLibraryPath { get; set; }

    /// <summary>
    /// Native library base name (without prefix or extension). The
    /// platform-specific filename is resolved by .NET's
    /// <c>NativeLibrary.Load</c>:
    /// <c>whiteout_native.dll</c> on Windows, <c>libwhiteout_native.so</c>
    /// on Linux, <c>libwhiteout_native.dylib</c> on macOS.
    /// </summary>
    /// <remarks>
    /// The same artefact backs every WhiteoutLib language binding (Java
    /// FFM, C# P/Invoke, Rust, ctypes-style Python). When a Java consumer
    /// has already staged it for a JVM run, the C# resolver picks the
    /// same file up — no parallel build needed.
    /// </remarks>
    public const string LibraryName = "whiteout_native";
}
