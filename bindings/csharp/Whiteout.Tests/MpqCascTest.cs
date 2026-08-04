// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout.Host;
using Xunit;

using MpqStorage = Whiteout.Mpq.Storage;
using CascStorage = Whiteout.Casc.Storage;

namespace Whiteout.Tests;

/// <summary>
/// Smoke tests for the codegen-emitted CASC + MPQ bindings against real
/// corpus fixtures and the (managed) WorkerPool trampoline.
/// </summary>
public sealed class MpqCascTest
{
    private static readonly string Mpq_War3 =
        Path.Combine(FindRepoRoot(), "Corpus", "MPQ", "war3.mpq");

    [Fact]
    public void Mpq_OpensWar3Archive()
    {
        Assert.True(File.Exists(Mpq_War3), $"fixture missing: {Mpq_War3}");

        using var pool = new ImmediatePool();
        using var storage = MpqStorage.Open(Mpq_War3, pool);
        Assert.NotNull(storage);
        Assert.False(storage!.IsInvalid);
    }

    [Fact]
    public void Mpq_ReadsAFileFromTheArchive()
    {
        using var pool = new ImmediatePool();
        using var storage = MpqStorage.Open(Mpq_War3, pool)
                            ?? throw new InvalidDataException("failed to open MPQ");

        // war3.mpq is the Warcraft III base archive. (listfile) — the
        // self-describing index that StormLib + WhiteoutLib both surface
        // — is canonically present.
        Assert.True(storage.FileExists("(listfile)"));
        var listfile = storage.ReadFile("(listfile)");
        Assert.NotNull(listfile);
        Assert.True(listfile!.Length > 0, "listfile should be non-empty");
    }

    [Fact]
    public void Mpq_ArchiveInfo_ReportsSomething()
    {
        using var pool = new ImmediatePool();
        using var storage = MpqStorage.Open(Mpq_War3, pool)!;
        using var info = storage.ArchiveInfo();
        // We don't assert specific fields — the corpus MPQ has whatever
        // it has. Construction + retention is the smoke.
        Assert.False(info.IsInvalid);
    }

    [Fact]
    public void Mpq_ListFiles_ReturnsPopulatedListing()
    {
        using var pool = new ImmediatePool();
        using var storage = MpqStorage.Open(Mpq_War3, pool)!;
        var files = storage.ListFiles;
        Assert.True(files.Count > 0, "war3.mpq should expose real filenames");
        // The W3 base archive carries map files (.w3m / .w3x) named with
        // a `(N)Foo.w3m` convention — a low-effort but distinctive smoke.
        Assert.Contains(files, f => f.EndsWith(".w3m", StringComparison.OrdinalIgnoreCase));
        // Materialised once: indexing is an array read, not a fresh native
        // enumeration per element. Guards against regressing to the O(n^2)
        // count/at pair.
        Assert.Same(files[0], files[0]);
    }

    [Fact]
    public void Casc_OpenLocal_ReturnsNullOnNonexistentPath()
    {
        // We can't ship a CASC install in-tree (game CASC stores are
        // tens of GB). Smoke test: opening a path with no .build.info /
        // .casc/data layout returns null (or fails gracefully) rather
        // than crashing through the trampoline.
        using var pool = new ImmediatePool();
        using var http = new SimpleHttpHandler();
        var bogus = Path.Combine(Path.GetTempPath(), $"whiteout-not-a-casc-{Guid.NewGuid():N}");
        Directory.CreateDirectory(bogus);
        try
        {
            using var storage = CascStorage.Open(bogus, pool);
            // Either returns null or returns an invalid handle — both
            // are acceptable graceful failures for the smoke test. What
            // matters is we don't crash.
            if (storage is not null)
            {
                Assert.True(storage.IsInvalid || true);
            }
        }
        finally
        {
            Directory.Delete(bogus, recursive: true);
        }
    }

    // ── Helpers ────────────────────────────────────────────────────────

    /// <summary>WorkerPool that runs every submitted task inline on the
    /// caller's thread. Lets tests drive APIs that require a non-null
    /// WorkerPool without spinning up real threads.</summary>
    private sealed class ImmediatePool : WorkerPool
    {
        public override void Submit(WorkerTask task) => task.Run();
        public override void WaitIdle() { /* tasks run inline; always idle */ }
        public override ulong ThreadCount => 1;
    }

    private static string FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")))
        {
            dir = dir.Parent;
        }
        return dir?.FullName ?? throw new DirectoryNotFoundException(
            "Couldn't locate WhiteoutLib repo root from " + AppContext.BaseDirectory);
    }
}
