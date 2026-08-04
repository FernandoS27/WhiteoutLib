// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout.Textures;
using Xunit;

namespace Whiteout.Tests;

/// <summary>
/// Binding smoke tests for the TEX parser. The D3/D4 TEX fixtures live in an
/// out-of-tree corpus (<c>Models/D4</c>), so these cover the P/Invoke surface
/// and the graceful-failure paths rather than real decode output.
/// </summary>
public sealed class TexParserTest
{
    [Fact]
    public void Construction_And_Disposal_Work()
    {
        using var parser = new TexParser();
        Assert.False(parser.IsInvalid);
    }

    [Fact]
    public void ParseBuffer_RejectsGarbageWithoutCrashing()
    {
        using var parser = new TexParser();
        var garbage = new byte[64];
        for (int i = 0; i < garbage.Length; i++) garbage[i] = (byte)i;

        Assert.Null(parser.ParseBuffer(garbage));
        Assert.NotNull(parser.Issues);
    }

    [Fact]
    public void ParseBuffer_RejectsEmptyInput()
    {
        using var parser = new TexParser();
        Assert.Null(parser.ParseBuffer(ReadOnlySpan<byte>.Empty));
    }

    [Fact]
    public void Parse_ReturnsNullForMissingFile()
    {
        using var parser = new TexParser();
        var missing = Path.Combine(Path.GetTempPath(), $"whiteout-no-such-{Guid.NewGuid():N}.tex");
        Assert.Null(parser.Parse(missing));
    }

    [Fact]
    public void ParseTexDataPayloadData_RejectsGarbageWithoutCrashing()
    {
        using var parser = new TexParser();
        Assert.Null(parser.ParseTexDataPayloadData(new byte[32], new byte[32]));
    }
}

/// <summary>
/// The parallel-decode constructors take a WorkerPool. They were unbindable
/// in C# until the ctor emitter learned to pass trampolined interfaces
/// through, so these pin that the overloads exist and actually run work on
/// the supplied pool.
/// </summary>
public sealed class PoolConstructorTest
{
    private sealed class CountingPool : Whiteout.Host.WorkerPool
    {
        private int _threadCountQueries;
        public int ThreadCountQueries => Volatile.Read(ref _threadCountQueries);

        public override void Submit(Whiteout.Host.WorkerTask task) => task.Run();
        public override void WaitIdle() { }

        // Reporting 1 keeps C++ on its single-threaded path — the managed
        // trampoline has no createTimelineSemaphore, which the parallel
        // path would need. The getter still fires, which is what proves
        // the handle crossed.
        public override ulong ThreadCount
        {
            get
            {
                Interlocked.Increment(ref _threadCountQueries);
                return 1;
            }
        }
    }

    [Fact]
    public void BlpWriter_AcceptsAWorkerPool()
    {
        using var pool = new CountingPool();
        using var writer = new Whiteout.Textures.BlpWriter(pool);
        Assert.False(writer.IsInvalid);
    }

    [Fact]
    public void JpegParser_AcceptsAWorkerPool()
    {
        using var pool = new CountingPool();
        using var parser = new Whiteout.Textures.JpegParser(pool);
        Assert.False(parser.IsInvalid);
    }

    [Fact]
    public void JpegWriter_AcceptsQualityPoolAndProgressive()
    {
        using var pool = new CountingPool();
        // Also pins the bool->int32 marshalling of the trailing param.
        using var writer = new Whiteout.Textures.JpegWriter(90, pool, progressive: true);
        Assert.False(writer.IsInvalid);
    }

    [Fact]
    public void PoolCtors_AcceptNullToMeanNoPool()
    {
        using var writer = new Whiteout.Textures.BlpWriter(null);
        Assert.False(writer.IsInvalid);
    }

    [Fact]
    public void PoolCtor_ActuallyHandsTheHandleToCpp()
    {
        using var pool = new CountingPool();
        using var writer = new Whiteout.Textures.JpegWriter(90, pool, progressive: false);
        using var texture = Whiteout.Textures.Texture.Create2D(
            Whiteout.Textures.PixelFormat.RGBA8, 64, 64, 1);

        var jpeg = writer.Write(texture);
        Assert.NotEmpty(jpeg);
        // The encoder consults pool->threadCount() to decide whether to go
        // parallel. That call is a trampoline back into managed code, so a
        // non-zero count is proof the pool reached C++ through the ctor
        // rather than being silently dropped.
        Assert.True(pool.ThreadCountQueries > 0,
                    "expected C++ to query the supplied pool's ThreadCount");
    }
}
