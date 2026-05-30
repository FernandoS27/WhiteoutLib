// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout.Host;
using Xunit;

namespace Whiteout.Tests;

/// <summary>
/// Smoke tests for the codegen-emitted concrete implementations under
/// <c>Whiteout.Host</c>:
/// <see cref="OsFileSystem"/>, <see cref="SimpleThreadPool"/>,
/// <see cref="SimpleHttpHandler"/>. These wrap the same C++ classes used
/// by C++ tooling (<c>WhiteoutTex</c>, the example programs).
/// </summary>
public sealed class HostConcreteImplsTest
{
    [Fact]
    public void OsFileSystem_ReadsAFileFromDisk()
    {
        var dir  = Path.Combine(Path.GetTempPath(), $"whiteout-tests-{Guid.NewGuid():N}");
        var path = Path.Combine(dir, "hello.txt");
        Directory.CreateDirectory(dir);
        File.WriteAllText(path, "hello whiteout");
        try
        {
            using var fs = new OsFileSystem(dir);
            Assert.True(fs.FileExists("hello.txt"));
            var bytes = fs.ReadFile("hello.txt");
            Assert.Equal("hello whiteout", System.Text.Encoding.UTF8.GetString(bytes));
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void OsFileSystem_MissingFileReportsAsNonExistent()
    {
        var dir = Path.Combine(Path.GetTempPath(), $"whiteout-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        try
        {
            using var fs = new OsFileSystem(dir);
            Assert.False(fs.FileExists("nope.txt"));
            Assert.Empty(fs.ReadFile("nope.txt"));
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void SimpleThreadPool_ConstructsWithThreadCount()
    {
        using var pool = new SimpleThreadPool(4);
        Assert.False(pool.IsInvalid);
        Assert.Equal(4ul, pool.ThreadCount);
    }

    [Fact]
    public void SimpleThreadPool_WaitIdleIsImmediate_WhenEmpty()
    {
        using var pool = new SimpleThreadPool(2);
        pool.WaitIdle();    // No tasks queued — must return promptly.
    }

    [Fact]
    public void SimpleHttpHandler_ConstructsBothCtorOverloads()
    {
        using var defaultHandler = new SimpleHttpHandler();
        using var threadedHandler = new SimpleHttpHandler(4);
        Assert.False(defaultHandler.IsInvalid);
        Assert.False(threadedHandler.IsInvalid);
    }
}
