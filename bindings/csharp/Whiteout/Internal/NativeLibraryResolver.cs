// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Whiteout.Internal;

/// <summary>
/// Registers a <see cref="NativeLibrary.SetDllImportResolver"/> for the
/// Whiteout assembly so non-NuGet consumers can point at a custom
/// whiteout_native binary via <see cref="Whiteout.Runtime.NativeLibraryPath"/>
/// or the <c>WHITEOUT_NATIVE_PATH</c> environment variable.
/// </summary>
internal static class NativeLibraryResolver
{
    private const string EnvVar = "WHITEOUT_NATIVE_PATH";

    // CA2255 warns against [ModuleInitializer] in library code because consumers
    // can't see the side effect. In our case the side effect is registering a
    // resolver for our OWN P/Invoke surface — the canonical use case Microsoft
    // documents in the NativeLibrary.SetDllImportResolver guidance. Suppressed
    // locally so the analyzer stays on for the rest of the assembly.
#pragma warning disable CA2255
    [ModuleInitializer]
    internal static void Initialize()
    {
        NativeLibrary.SetDllImportResolver(typeof(NativeLibraryResolver).Assembly, Resolve);
    }
#pragma warning restore CA2255

    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName != Runtime.LibraryName)
        {
            return IntPtr.Zero;
        }

        // Priority 1: explicit static override.
        var overridePath = Runtime.NativeLibraryPath;
        if (!string.IsNullOrEmpty(overridePath) && NativeLibrary.TryLoad(overridePath, out var h1))
        {
            return h1;
        }

        // Priority 2: environment variable.
        var envPath = Environment.GetEnvironmentVariable(EnvVar);
        if (!string.IsNullOrEmpty(envPath) && NativeLibrary.TryLoad(envPath, out var h2))
        {
            return h2;
        }

        // Priority 3: fall through to .NET's default loader (which finds
        // NuGet runtimes/<rid>/native/ payloads and the OS search path).
        return IntPtr.Zero;
    }
}
