// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout.Textures;
using Xunit;

namespace Whiteout.Tests;

/// <summary>
/// Round-trip coverage for the Diablo II: Resurrected `.texture` bindings.
/// Mirrors the synthetic case in tests/d2r_texture_test.cpp — BC3 is the
/// format the container supports.
/// </summary>
public sealed class D2rTextureTest
{
    private static Texture MakeBc3(uint width, uint height, uint mips) =>
        Texture.Create2D(PixelFormat.BC3, width, height, mips);

    [Fact]
    public void WriteThenParse_PreservesDimensionsAndFormat()
    {
        using var source = MakeBc3(4, 4, 2);
        using var writer = new D2rTextureWriter();

        var bytes = writer.Write(source);
        Assert.NotEmpty(bytes);

        using var parser = new D2rTextureParser();
        using var parsed = parser.Parse(bytes);

        Assert.NotNull(parsed);
        Assert.Equal(source.Width, parsed!.Width);
        Assert.Equal(source.Height, parsed.Height);
        Assert.Equal(PixelFormat.BC3, parsed.Format);
        Assert.Equal(source.MipCount, parsed.MipCount);
    }

    [Fact]
    public void Detect_AcceptsWrittenOutputAndRejectsGarbage()
    {
        using var source = MakeBc3(8, 8, 1);
        using var writer = new D2rTextureWriter();
        var bytes = writer.Write(source);

        using var parser = new D2rTextureParser();
        Assert.True(parser.Detect(bytes));

        var garbage = new byte[64];
        for (int i = 0; i < garbage.Length; i++) garbage[i] = (byte)i;
        Assert.False(parser.Detect(garbage));
    }

    [Fact]
    public void Parse_RejectsGarbageWithoutCrashing()
    {
        using var parser = new D2rTextureParser();
        Assert.Null(parser.Parse(new byte[64]));
        Assert.Null(parser.Parse(ReadOnlySpan<byte>.Empty));
    }

    [Fact]
    public void Write_RejectsAnUnsupportedPixelFormat()
    {
        // The container has no code for BC7 — the writer reports issues and
        // yields nothing rather than emitting a bogus file.
        using var texture = Texture.Create2D(PixelFormat.BC7, 8, 8, 1);
        using var writer = new D2rTextureWriter();

        Assert.Empty(writer.Write(texture));
        Assert.True(writer.HasIssues);
        Assert.NotEmpty(writer.Issues);
    }
}
