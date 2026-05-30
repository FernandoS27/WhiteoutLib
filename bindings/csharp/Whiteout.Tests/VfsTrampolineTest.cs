// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout.Host;
using Xunit;

using M2Parser = Whiteout.M2.Parser;

namespace Whiteout.Tests;

/// <summary>
/// Round-trip tests for the C# → C++ → C# trampoline on
/// <see cref="VirtualPathFileSystem"/>. A managed subclass plugs into
/// a C++ shim; the test invokes the virtual methods via the C ABI's
/// smoke-test entry points, which is the same path library code (e.g.
/// the M2 parser) would use.
/// </summary>
/// <remarks>
/// Requires <c>whiteout_c.dll</c> on the search path or pointed at by
/// <c>WHITEOUT_NATIVE_PATH</c>.
/// </remarks>
public sealed class VfsTrampolineTest
{
    [Fact]
    public void Construct_And_Dispose()
    {
        using var vfs = new InMemoryVfs();
        Assert.False(vfs.IsInvalid);
    }

    [Fact]
    public void ReadFile_RoundTripsThroughCppShim()
    {
        var contents = new byte[] { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03 };
        var vfs = new InMemoryVfs();
        vfs.Add("model.m2", contents);

        // InvokeReadFile calls back into managed code via the C++ shim:
        //   C# test → P/Invoke → C++ vfs->readFile(path) → C# trampoline
        //   → managed ReadFile(path) → byte[] → C++ vector<u8> → whiteout_Bytes
        //   → managed byte[]
        var got = vfs.InvokeReadFileViaTrampoline("model.m2");
        Assert.Equal(contents, got);

        vfs.Dispose();
    }

    [Fact]
    public void ReadFile_MissingFile_ReturnsEmpty()
    {
        using var vfs = new InMemoryVfs();
        var got = vfs.InvokeReadFileViaTrampoline("does-not-exist.m2");
        Assert.Empty(got);
    }

    [Fact]
    public void FileExists_RoundTripsThroughCppShim()
    {
        using var vfs = new InMemoryVfs();
        vfs.Add("hello.txt", new byte[] { (byte)'h' });

        Assert.True(vfs.InvokeFileExistsViaTrampoline("hello.txt"));
        Assert.False(vfs.InvokeFileExistsViaTrampoline("missing.txt"));
    }

    [Fact]
    public void WriteFile_RoundTripsThroughCppShim()
    {
        using var vfs = new InMemoryVfs();
        Assert.True(vfs.InvokeWriteFileViaTrampoline("out.bin", new byte[] { 1, 2, 3, 4 }));
        Assert.Equal(new byte[] { 1, 2, 3, 4 }, vfs.InvokeReadFileViaTrampoline("out.bin"));
    }

    [Fact]
    public void M2Parser_DrivesManagedVfs_ThroughFullFFIChain()
    {
        // Proves the end-to-end path that actually matters:
        //
        //   C# test → M2 parser → C++ m2::Parser::parse(VFS&, path)
        //     → C++ calls VFS::readFile virtual
        //     → C++ shim's override fires the readFile fn pointer
        //     → C# trampoline runs, calls managed ReadFile
        //     → bytes flow back to C++
        //
        // We can't ship an M2 fixture in-tree, so the test uses a stub VFS
        // whose ReadFile returns garbage. The parse will fail (and that's
        // fine) — what we verify is that ReadFile was *called* with the
        // right path. If the trampoline weren't wired correctly, the C++
        // side would never make it that far.
        var vfs = new RecordingVfs();
        using var parser = new M2Parser();
        using var _ = parser.Parse(vfs, "models/test.m2");

        Assert.Contains("models/test.m2", vfs.PathsRead);

        vfs.Dispose();
    }

    [Fact]
    public void ManagedException_DoesNotCrossBoundary()
    {
        // The trampoline catches managed exceptions and returns a safe
        // default (empty buffer / false) — proves we'll never tear down
        // the process if user code throws.
        using var vfs = new ThrowingVfs();
        var got = vfs.InvokeReadFileViaTrampoline("anything");
        Assert.Empty(got);
        Assert.False(vfs.InvokeFileExistsViaTrampoline("anything"));
    }

    // ── Helper impls ────────────────────────────────────────────────

    /// <summary>Minimal in-memory VFS backed by a Dictionary.</summary>
    private sealed class InMemoryVfs : VirtualPathFileSystem
    {
        private readonly Dictionary<string, byte[]> _files = new();

        public void Add(string path, byte[] data) => _files[path] = data;

        public override byte[] ReadFile(string path) =>
            _files.TryGetValue(path, out var data) ? data : Array.Empty<byte>();

        public override bool WriteFile(string path, ReadOnlySpan<byte> data)
        {
            _files[path] = data.ToArray();
            return true;
        }

        public override bool FileExists(string path) => _files.ContainsKey(path);
    }

    /// <summary>VFS stub that records every readFile path the C++ side
    /// asks for, but returns 32 bytes of garbage as the contents.</summary>
    private sealed class RecordingVfs : VirtualPathFileSystem
    {
        public List<string> PathsRead { get; } = new();

        public override byte[] ReadFile(string path)
        {
            PathsRead.Add(path);
            return new byte[32];
        }

        public override bool WriteFile(string path, ReadOnlySpan<byte> data) => true;
        public override bool FileExists(string path) => true;
    }

    private sealed class ThrowingVfs : VirtualPathFileSystem
    {
        public override byte[] ReadFile(string path) =>
            throw new InvalidOperationException("simulated user error");

        public override bool WriteFile(string path, ReadOnlySpan<byte> data) =>
            throw new InvalidOperationException("simulated user error");

        public override bool FileExists(string path) =>
            throw new InvalidOperationException("simulated user error");
    }
}
