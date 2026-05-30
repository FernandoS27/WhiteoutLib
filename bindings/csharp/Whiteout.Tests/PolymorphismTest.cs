// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout.Host;
using Xunit;

using MpqStorage = Whiteout.Mpq.Storage;

namespace Whiteout.Tests;

/// <summary>
/// The codegen-emitted concrete host impls (<see cref="OsFileSystem"/>,
/// <see cref="SimpleThreadPool"/>, <see cref="SimpleHttpHandler"/>) now
/// inherit from the matching managed trampoline base classes, so they
/// flow into APIs that expect the abstract bases directly — no adapter
/// wrapper needed.
/// </summary>
public sealed class PolymorphismTest
{
    [Fact]
    public void SimpleThreadPool_IsA_WorkerPool()
    {
        using var pool = new SimpleThreadPool(2);
        Assert.IsAssignableFrom<WorkerPool>(pool);
    }

    [Fact]
    public void OsFileSystem_IsA_VirtualPathFileSystem()
    {
        var dir = Path.Combine(Path.GetTempPath(), $"whiteout-poly-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        try
        {
            using var fs = new OsFileSystem(dir);
            Assert.IsAssignableFrom<VirtualPathFileSystem>(fs);
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void SimpleHttpHandler_IsA_HttpHandler()
    {
        using var http = new SimpleHttpHandler();
        Assert.IsAssignableFrom<HttpHandler>(http);
    }

    [Fact]
    public void SimpleThreadPool_PassesAsWorkerPool_To_MpqStorageOpen()
    {
        // The headline polymorphism win: the codegen-emitted concrete
        // pool can be handed directly to Mpq.Storage.Open(string, WorkerPool)
        // without writing an adapter.
        var path = Path.Combine(FindRepoRoot(), "Corpus", "MPQ", "war3.mpq");
        if (!File.Exists(path)) return;  // fixture skip

        using var pool = new SimpleThreadPool(2);
        using var storage = MpqStorage.Open(path, pool);
        Assert.NotNull(storage);
    }

    [Fact]
    public void OsFileSystem_DispatchesNatively_ToCppOsFileSystem()
    {
        // OsFileSystem inherits from VirtualPathFileSystem, but its
        // construction takes the (IntPtr, bool) path so no managed
        // trampoline is set up. Public ReadFile is the codegen override
        // calling whiteout_host_OsFileSystem_readFile directly. Prove
        // both invocation paths see the same file contents:
        //   1. The override on OsFileSystem (direct native call)
        //   2. The base-class invoker (C++ virtual dispatch via the shim)
        var dir = Path.Combine(Path.GetTempPath(), $"whiteout-dispatch-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "x.txt"), "shared");
        try
        {
            using var fs = new OsFileSystem(dir);
            var viaOverride  = fs.ReadFile("x.txt");
            var viaShimPath  = fs.InvokeReadFileViaTrampoline("x.txt");
            Assert.Equal(viaOverride, viaShimPath);
            Assert.Equal("shared", System.Text.Encoding.UTF8.GetString(viaOverride));
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
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
