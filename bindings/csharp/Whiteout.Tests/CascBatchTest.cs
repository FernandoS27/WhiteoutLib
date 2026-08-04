// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout.Casc;
using Whiteout.Host;
using Xunit;

using CascStorage = Whiteout.Casc.Storage;

namespace Whiteout.Tests;

/// <summary>
/// Round-trip coverage for the CASC surfaces that don't go through the plain
/// codegen path: <c>ListEntries</c> (snapshot-backed record list),
/// <c>ReadBatch</c> (hand-written shim), and the nullable <c>optional&lt;T&gt;</c>
/// returns. A writable storage is created, saved and reopened, so none of this
/// needs a real game install.
/// </summary>
public sealed class CascBatchTest : IDisposable
{
    private readonly string _dir = Path.Combine(
        Path.GetTempPath(), $"whiteout-casc-batch-{Guid.NewGuid():N}");

    private static byte[] MakeData(int size, byte seed)
    {
        var data = new byte[size];
        for (int i = 0; i < size; i++) data[i] = (byte)((i + seed) & 0xFF);
        return data;
    }

    /// <summary>Build a storage holding three files and reopen it from disk.</summary>
    private CascStorage OpenRoundTripped(WorkerPool pool, out Dictionary<string, byte[]> written)
    {
        written = new Dictionary<string, byte[]>
        {
            ["dir/file1.txt"] = MakeData(1024, 0x11),
            ["dir/file2.bin"] = MakeData(65536, 0x22),   // spans multiple BLTE frames
            ["tiny.dat"] = MakeData(1, 0x33),
        };

        using (var opts = new CreateOptions { Product = "test", Version = "1.0.0" })
        using (var writeOpts = new WriteOptions())
        using (var writable = StorageWritable.Create(opts, pool))
        {
            foreach (var (path, data) in written)
                Assert.True(writable.WriteFile(path, data, writeOpts), $"writeFile failed: {path}");
            Assert.True(writable.SavePath(_dir), "save failed");
        }

        return CascStorage.Open(_dir, pool)
               ?? throw new InvalidDataException($"failed to reopen CASC storage at {_dir}");
    }

    /// <summary>CASC stores paths with backslash separators.</summary>
    private static string Normalize(string path) => path.Replace('/', '\\');

    [Fact]
    public void ListEntries_ReturnsMetadataForEveryWrittenFile()
    {
        using var pool = new ImmediatePool();
        using var storage = OpenRoundTripped(pool, out var written);

        var entries = storage.ListEntries();
        Assert.NotEmpty(entries);

        foreach (var (path, data) in written)
        {
            var entry = Assert.Single(
                entries.Where(e => string.Equals(
                    e.Path, Normalize(path), StringComparison.OrdinalIgnoreCase)));
            Assert.Equal((ulong)data.Length, entry.FileSize);
            // cKey is a 16-byte content-key field, lowered through the
            // snapshot's Bytes accessor. Root-manifest entries carry the
            // 9-byte truncated form zero-padded to 16, so only assert the
            // width and that something was actually written.
            Assert.Equal(16, entry.CKey.Length);
            Assert.NotEqual(new byte[16], entry.CKey);
        }
    }

    [Fact]
    public void ListEntries_IsRepeatableAndDoesNotCorruptTheSnapshot()
    {
        using var pool = new ImmediatePool();
        using var storage = OpenRoundTripped(pool, out _);

        var first = storage.ListEntries();
        var second = storage.ListEntries();

        Assert.Equal(first.Count, second.Count);
        Assert.Equal(
            first.Select(e => e.Path).OrderBy(p => p, StringComparer.Ordinal),
            second.Select(e => e.Path).OrderBy(p => p, StringComparer.Ordinal));
    }

    [Fact]
    public void ReadBatch_ReadsEveryRequestedFileInOrder()
    {
        using var pool = new ImmediatePool();
        using var storage = OpenRoundTripped(pool, out var written);

        var paths = written.Keys.ToList();
        var results = storage.ReadBatch(paths.Select(BatchReadRequest.ByPath).ToList());

        Assert.Equal(paths.Count, results.Count);
        for (int i = 0; i < paths.Count; i++)
        {
            Assert.True(results[i].Success, $"batch read failed for {paths[i]}: {results[i].Error}");
            Assert.Equal(written[paths[i]], results[i].Data);
        }
    }

    [Fact]
    public void ReadBatch_ReportsPerFileFailureWithoutAffectingTheOthers()
    {
        using var pool = new ImmediatePool();
        using var storage = OpenRoundTripped(pool, out var written);

        var results = storage.ReadBatch(new[]
        {
            BatchReadRequest.ByPath("tiny.dat"),
            BatchReadRequest.ByPath("does/not/exist.bin"),
            BatchReadRequest.ByPath("dir/file1.txt"),
        });

        Assert.Equal(3, results.Count);
        Assert.True(results[0].Success);
        Assert.Equal(written["tiny.dat"], results[0].Data);
        Assert.False(results[1].Success);
        Assert.Null(results[1].Data);
        Assert.True(results[2].Success);
        Assert.Equal(written["dir/file1.txt"], results[2].Data);
    }

    [Fact]
    public void ListFiles_MaterialisesOnceInsteadOfPerIndex()
    {
        using var pool = new ImmediatePool();
        using var storage = OpenRoundTripped(pool, out var written);

        var files = storage.ListFiles;
        Assert.Equal(written.Count, files.Count);
        foreach (var path in written.Keys)
            Assert.Contains(Normalize(path), files);

        // The property returns a materialised snapshot, so repeated indexing
        // is a managed array read rather than a fresh native enumeration.
        // Two reads of the same index must be reference-equal strings from
        // the same snapshot instance.
        Assert.Same(files[0], files[0]);
    }

    [Fact]
    public void ReadBatch_EmptyRequestListReturnsEmpty()
    {
        using var pool = new ImmediatePool();
        using var storage = OpenRoundTripped(pool, out _);

        Assert.Empty(storage.ReadBatch(Array.Empty<BatchReadRequest>()));
    }

    [Fact]
    public void OptionalScalarReturns_MapToNullableValues()
    {
        using var pool = new ImmediatePool();
        using var storage = OpenRoundTripped(pool, out var written);

        // Present → the value; absent → null. The C ABI signals this with a
        // has-value flag plus an out-param, so a legitimate 0 stays distinct
        // from "not found".
        Assert.Equal((ulong)written["tiny.dat"].Length, storage.FileSize("tiny.dat"));
        Assert.Null(storage.FileSize("does/not/exist.bin"));

        var total = storage.TotalFileCount();
        Assert.True(total is null or > 0);
    }

    public void Dispose()
    {
        try
        {
            if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
        }
        catch (IOException)
        {
            // Best-effort cleanup; a leftover temp dir shouldn't fail the run.
        }
    }

    /// <summary>WorkerPool that runs every submitted task inline on the
    /// caller's thread.</summary>
    private sealed class ImmediatePool : WorkerPool
    {
        public override void Submit(WorkerTask task) => task.Run();
        public override void WaitIdle() { }
        public override ulong ThreadCount => 1;
    }
}
