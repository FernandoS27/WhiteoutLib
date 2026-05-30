// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Whiteout.Common;
using Whiteout.Host.Internal;

namespace Whiteout.Host;

/// <summary>
/// Managed base class for the C++ <c>whiteout::interfaces::VirtualPathFileSystem</c>.
/// </summary>
/// <remarks>
/// <para><b>Two construction paths:</b></para>
/// <list type="bullet">
///   <item><b>User trampoline subclass.</b> The parameterless constructor
///   allocates a <see cref="GCHandle"/> and a C++ shim that forwards
///   virtual calls back into managed <see cref="ReadFile"/>/
///   <see cref="WriteFile"/>/<see cref="FileExists"/> overrides.</item>
///   <item><b>Codegen-generated concrete impl</b> (e.g. <c>OsFileSystem</c>).
///   The (IntPtr, bool) constructor wraps an already-constructed native
///   handle directly — no trampoline shim is created, and the abstracts
///   are overridden with direct C ABI calls to the concrete impl's
///   methods. The C++ side's virtual dispatch goes straight to the
///   concrete impl without ever crossing back to managed code.</item>
/// </list>
/// <para>Both paths share the same public API surface, so a value of
/// type <c>VirtualPathFileSystem</c> behaves the same way regardless of
/// whether it's a managed subclass or a wrapped native impl.</para>
/// </remarks>
public abstract unsafe class VirtualPathFileSystem : WhiteoutHandle
{
    [StructLayout(LayoutKind.Sequential)]
    private struct FnTable
    {
        public delegate* unmanaged<IntPtr, byte*, nuint, byte**, nuint*, void> ReadFile;
        public delegate* unmanaged<byte*, void> FreeBuffer;
        public delegate* unmanaged<IntPtr, byte*, nuint, byte*, nuint, int> WriteFile;
        public delegate* unmanaged<IntPtr, byte*, nuint, int> FileExists;
    }

    private static readonly FnTable s_fnTable = new()
    {
        ReadFile   = &ReadFileTrampoline,
        FreeBuffer = &FreeBufferTrampoline,
        WriteFile  = &WriteFileTrampoline,
        FileExists = &FileExistsTrampoline,
    };

    private GCHandle _self;

    /// <summary>Trampoline constructor — for user subclasses that
    /// implement the VFS in managed code.</summary>
    protected VirtualPathFileSystem()
    {
        _self = GCHandle.Alloc(this);
        fixed (FnTable* tablePtr = &Unsafe.AsRef(in s_fnTable))
        {
            var nativeHandle = NativeShims.whiteout_csharp_VirtualPathFileSystem_create(
                GCHandle.ToIntPtr(_self), tablePtr);
            SetHandle(nativeHandle);
        }
    }

    /// <summary>Native-handle constructor — for codegen-generated concrete
    /// impls (<c>OsFileSystem</c>, etc.) that wrap an already-constructed
    /// C++ implementation. No GCHandle, no trampoline shim.</summary>
    protected internal VirtualPathFileSystem(IntPtr nativeHandle, bool owned)
        : base(nativeHandle, owned)
    {
        // _self stays default — IsAllocated returns false, so ReleaseHandle
        // skips the GCHandle.Free path.
    }

    /// <summary>Read the full contents of <paramref name="path"/>.
    /// Returns an empty array when the file is missing or unreadable.</summary>
    public abstract byte[] ReadFile(string path);

    /// <summary>Write <paramref name="data"/> to <paramref name="path"/>.
    /// Returns <c>true</c> on success.</summary>
    public abstract bool WriteFile(string path, ReadOnlySpan<byte> data);

    /// <summary>Whether <paramref name="path"/> exists.</summary>
    public abstract bool FileExists(string path);

    protected override bool ReleaseHandle()
    {
        if (handle != IntPtr.Zero)
        {
            // Only the trampoline construction path produced a CsharpVFS
            // shim on the C++ side — detect it via GCHandle ownership.
            // For codegen concrete impls (no GCHandle), the codegen's
            // override does the right delete in its own ReleaseHandle.
            if (_self.IsAllocated)
            {
                NativeShims.whiteout_csharp_VirtualPathFileSystem_delete(handle);
            }
        }
        if (_self.IsAllocated)
        {
            _self.Free();
        }
        return true;
    }

    [UnmanagedCallersOnly]
    private static void ReadFileTrampoline(IntPtr userdata, byte* pathPtr, nuint pathLen,
                                           byte** outData, nuint* outSize)
    {
        try
        {
            var instance = (VirtualPathFileSystem)GCHandle.FromIntPtr(userdata).Target!;
            var path = Encoding.UTF8.GetString(pathPtr, checked((int)pathLen));
            var data = instance.ReadFile(path);
            if (data is null || data.Length == 0)
            {
                *outData = null;
                *outSize = 0;
                return;
            }
            var buf = (byte*)NativeMemory.Alloc((nuint)data.Length);
            fixed (byte* src = data)
            {
                NativeMemory.Copy(src, buf, (nuint)data.Length);
            }
            *outData = buf;
            *outSize = (nuint)data.Length;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Whiteout VFS.ReadFile] {ex}");
            *outData = null;
            *outSize = 0;
        }
    }

    [UnmanagedCallersOnly]
    private static void FreeBufferTrampoline(byte* buf)
    {
        NativeMemory.Free(buf);
    }

    [UnmanagedCallersOnly]
    private static int WriteFileTrampoline(IntPtr userdata, byte* pathPtr, nuint pathLen,
                                           byte* dataPtr, nuint dataLen)
    {
        try
        {
            var instance = (VirtualPathFileSystem)GCHandle.FromIntPtr(userdata).Target!;
            var path = Encoding.UTF8.GetString(pathPtr, checked((int)pathLen));
            var data = new ReadOnlySpan<byte>(dataPtr, checked((int)dataLen));
            return instance.WriteFile(path, data) ? 1 : 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Whiteout VFS.WriteFile] {ex}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    private static int FileExistsTrampoline(IntPtr userdata, byte* pathPtr, nuint pathLen)
    {
        try
        {
            var instance = (VirtualPathFileSystem)GCHandle.FromIntPtr(userdata).Target!;
            var path = Encoding.UTF8.GetString(pathPtr, checked((int)pathLen));
            return instance.FileExists(path) ? 1 : 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Whiteout VFS.FileExists] {ex}");
            return 0;
        }
    }

    /// <summary>Drive the C++ virtual dispatch path explicitly, regardless
    /// of how this instance was constructed. Useful for proving the
    /// trampoline mechanism works when running tests against a managed
    /// subclass.</summary>
    public byte[] InvokeReadFileViaTrampoline(string path)
        => NativeShims.whiteout_csharp_test_VirtualPathFileSystem_readFile(
                DangerousGet(), path).ToManagedArray();

    /// <summary>Same as <see cref="ReadFile"/> via the explicit native dispatch path.</summary>
    public bool InvokeFileExistsViaTrampoline(string path)
        => NativeShims.whiteout_csharp_test_VirtualPathFileSystem_fileExists(DangerousGet(), path);

    /// <summary>Same as <see cref="WriteFile"/> via the explicit native dispatch path.</summary>
    public bool InvokeWriteFileViaTrampoline(string path, ReadOnlySpan<byte> data)
        => NativeShims.whiteout_csharp_test_VirtualPathFileSystem_writeFile(
                DangerousGet(), path, data, (nuint)data.Length);
}
