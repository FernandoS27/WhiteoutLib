// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout;
using Whiteout.Textures;
using Xunit;

// Several modules have a `Parser` / `Model` class; alias to avoid collisions.
using MdxParser = Whiteout.Mdx.Parser;
using MdxWriter = Whiteout.Mdx.Writer;
using MdxModel  = Whiteout.Mdx.Model;
using MDLXFormat = Whiteout.Mdx.MDLXFormat;
using MdlFormat  = Whiteout.Mdx.MdlFormat;
using Extent     = Whiteout.Mdx.Extent;
using M3Parser   = Whiteout.M3.Parser;
using M3Model    = Whiteout.M3.Model;
using M2Parser   = Whiteout.M2.Parser;
using M2Model    = Whiteout.M2.Model;

namespace Whiteout.Tests;

/// <summary>
/// End-to-end tests against a built whiteout_c.dll.
/// </summary>
/// <remarks>
/// Requires whiteout_c.dll on the OS search path or pointed at via the
/// <c>WHITEOUT_NATIVE_PATH</c> env var / <see cref="Runtime.NativeLibraryPath"/>.
/// </remarks>
public sealed class SmokeTest
{
    // BLP / MDX fixtures from the corpus. Paths are relative to the repo root.
    private static readonly string BlpFixture =
        Path.Combine(FindRepoRoot(), "Corpus", "MDL", "Ace", "ace_acg1.blp");
    private static readonly string MdxFixture =
        Path.Combine(FindRepoRoot(), "Corpus", "MDL", "Ace", "Ace.mdx");
    private static readonly string M3Fixture =
        Path.Combine(FindRepoRoot(), "Corpus", "HotSM3", "LightOmniBlueLarge.m3");

    [Fact]
    public void ConstructAndDispose_Texture()
    {
        using var tex = new Texture();
        Assert.False(tex.IsInvalid);
    }

    [Fact]
    public void PixelFormat_EnumValuesMatchCpp()
    {
        Assert.Equal(0, (int)PixelFormat.R8);
    }

    [Fact]
    public void Texture_Create2D_ProducesValidTexture()
    {
        using var tex = Texture.Create2D(PixelFormat.RGBA8, 64, 32, 1);
        Assert.False(tex.IsInvalid);
        Assert.Equal(64u, tex.Width);
        Assert.Equal(32u, tex.Height);
        Assert.Equal(1u, tex.MipCount);
        Assert.Equal((uint)(64 * 32 * 4), (uint)tex.DataSize);
    }

    [Fact]
    public void BlpParser_ParsesRealBlpFile()
    {
        Assert.True(File.Exists(BlpFixture), $"fixture not found: {BlpFixture}");
        var blpBytes = File.ReadAllBytes(BlpFixture);

        using var parser = new BlpParser();
        using var tex = parser.Parse(blpBytes);

        Assert.NotNull(tex);
        Assert.True(tex.Width > 0, "width must be > 0");
        Assert.True(tex.Height > 0, "height must be > 0");
        Assert.True(tex.MipCount >= 1, "mip count must be >= 1");
    }

    [Fact]
    public void Blp_To_Png_RoundTrip()
    {
        var blpBytes = File.ReadAllBytes(BlpFixture);

        using var parser = new BlpParser();
        using var tex = parser.Parse(blpBytes) ?? throw new InvalidDataException();
        using var pngWriter = new PngWriter();
        var pngBytes = pngWriter.Write(tex);

        // PNG signature: 89 50 4E 47 0D 0A 1A 0A
        Assert.True(pngBytes.Length > 8);
        Assert.Equal(0x89, pngBytes[0]);
        Assert.Equal((byte)'P', pngBytes[1]);
        Assert.Equal((byte)'N', pngBytes[2]);
        Assert.Equal((byte)'G', pngBytes[3]);
    }

    [Fact]
    public void Texture_IsSrgb_RoundTripsBoolean()
    {
        using var tex = Texture.Create2D(PixelFormat.RGBA8, 4, 4, 1);
        tex.IsSrgb = true;
        Assert.True(tex.IsSrgb);
        tex.IsSrgb = false;
        Assert.False(tex.IsSrgb);
    }

    [Fact]
    public void Texture_Format_PropertyPairsCppOverloads()
    {
        // The C++ side has two `format()` overloads — a const no-arg
        // getter and a non-const setter taking PixelFormat. The codegen
        // folds them into a single property `Format { get; set; }`.
        using var tex = Texture.Create2D(PixelFormat.RGBA8, 4, 4, 1);
        Assert.Equal(PixelFormat.RGBA8, tex.Format);
        tex.Format = PixelFormat.RGBA16;
        Assert.Equal(PixelFormat.RGBA16, tex.Format);
    }

    [Fact]
    public void PngApngFrameInfo_PropertiesRoundTrip()
    {
        using var info = new PngApngFrameInfo();
        info.Width = 640;
        info.Height = 480;
        info.XOffset = 12;
        info.YOffset = 24;
        info.DelayMs = 100;

        Assert.Equal(640u, info.Width);
        Assert.Equal(480u, info.Height);
        Assert.Equal(12u, info.XOffset);
        Assert.Equal(24u, info.YOffset);
        Assert.Equal(100u, info.DelayMs);
    }

    [Fact]
    public void GifSaveOptions_BoolAndPrimitiveProperties()
    {
        using var opts = new GifSaveOptions();
        opts.LoopCount = 7;
        opts.Dither = true;
        opts.DitherStrength = 0.5f;
        opts.Transparent = false;

        Assert.Equal((ushort)7, opts.LoopCount);
        Assert.True(opts.Dither);
        Assert.Equal(0.5f, opts.DitherStrength);
        Assert.False(opts.Transparent);
    }

    [Fact]
    public void MdxParser_ParsesRealMdxFile()
    {
        Assert.True(File.Exists(MdxFixture), $"fixture not found: {MdxFixture}");
        var mdxBytes = File.ReadAllBytes(MdxFixture);

        using var parser = new MdxParser();
        using var model = parser.ParseBufferFormat(mdxBytes, MDLXFormat.MDX);

        Assert.NotNull(model);
        Assert.True(model.Version >= 800, $"expected MDX800+, got {model.Version}");
        Assert.False(string.IsNullOrEmpty(model.ModelName), "model name must be set");
    }

    [Fact]
    public void MdxModel_StringPropertyRoundTrips()
    {
        using var model = new MdxModel();
        model.ModelName = "RoundTripTest";
        Assert.Equal("RoundTripTest", model.ModelName);
    }

    [Fact]
    public void MdxModel_UintPropertyRoundTrips()
    {
        using var model = new MdxModel();
        model.Version = 1000;
        Assert.Equal(1000u, model.Version);
        model.BlendTime = 250;
        Assert.Equal(250u, model.BlendTime);
    }

    [Fact]
    public void MdxModel_VectorClassFields_ExposedAsIReadOnlyList()
    {
        var mdxBytes = File.ReadAllBytes(MdxFixture);
        using var parser = new MdxParser();
        using var model = parser.ParseBufferFormat(mdxBytes, MDLXFormat.MDX)
                          ?? throw new InvalidDataException();

        // Bones / Geosets / Sequences are now IReadOnlyList<T> properties
        // backed by the C ABI's count/at pair.
        Assert.True(model.Bones.Count > 0, "MDX fixture should have bones");
        Assert.True(model.Geosets.Count > 0, "MDX fixture should have geosets");
        Assert.True(model.Sequences.Count > 0, "MDX fixture should have sequences");

        // Indexer access — element is a borrowed view; the parent owns
        // the memory, so we don't dispose it directly.
        var firstBone = model.Bones[0];
        Assert.False(firstBone.IsInvalid);

        // foreach iteration works.
        var counted = 0;
        foreach (var _ in model.Bones) counted++;
        Assert.Equal(model.Bones.Count, counted);
    }

    [Fact]
    public void MdxModel_PivotPoints_ExposedAsZeroCopySpan()
    {
        var mdxBytes = File.ReadAllBytes(MdxFixture);
        using var parser = new MdxParser();
        using var model = parser.ParseBufferFormat(mdxBytes, MDLXFormat.MDX)
                          ?? throw new InvalidDataException();

        // PivotPoints is a vector<Vector3f> field — exposed as a zero-
        // copy ReadOnlySpan<System.Numerics.Vector3> aliased to the C++
        // vector's storage. No allocation, no per-element copy.
        var pivots = model.PivotPoints;
        Assert.True(pivots.Length > 0, "MDX should have pivot points");

        // Each element is a System.Numerics.Vector3 — usable directly
        // with any SIMD-friendly System.Numerics code.
        var first = pivots[0];
        Assert.IsType<System.Numerics.Vector3>(first);
    }

    [Fact]
    public void BlpParser_Issues_ExposedAsIReadOnlyList()
    {
        // A well-formed BLP should parse without issues. The Issues
        // surface (lowered from C++ `getIssues() -> vector<string>` via
        // the count + at(i) C ABI expansion) should be empty.
        var blpBytes = File.ReadAllBytes(BlpFixture);
        using var parser = new BlpParser();
        using var tex = parser.Parse(blpBytes);
        Assert.NotNull(tex);
        Assert.False(parser.HasIssues);
        Assert.Empty(parser.Issues);
    }

    [Fact]
    public void BlpParser_Issues_PopulatedOnMalformedInput()
    {
        // Garbage bytes — the parser should report at least one issue.
        var garbage = new byte[64];
        for (var i = 0; i < garbage.Length; i++) garbage[i] = (byte)i;

        using var parser = new BlpParser();
        var tex = parser.Parse(garbage);
        tex?.Dispose();

        Assert.True(parser.HasIssues);
        var issues = parser.Issues;
        Assert.NotEmpty(issues);
        Assert.All(issues, s => Assert.False(string.IsNullOrEmpty(s)));
    }

    [Fact]
    public void Extent_Construct()
    {
        using var extent = new Extent();
        Assert.False(extent.IsInvalid);
    }

    [Fact]
    public void Extent_ReadMinimum()
    {
        using var extent = new Extent();
        var min = extent.Minimum;  // Just read; should default-init to zero.
        Assert.Equal(0f, min.X);
    }

    [Fact]
    public void Extent_ScalarMathField_RoundTripsAsSystemNumericsVector3()
    {
        using var extent = new Extent();
        extent.Minimum = new System.Numerics.Vector3(-1f, -2f, -3f);
        extent.Maximum = new System.Numerics.Vector3( 4f,  5f,  6f);

        var min = extent.Minimum;
        Assert.Equal(-1f, min.X);
        Assert.Equal(-2f, min.Y);
        Assert.Equal(-3f, min.Z);

        var max = extent.Maximum;
        Assert.Equal(4f, max.X);
        Assert.Equal(5f, max.Y);
        Assert.Equal(6f, max.Z);
    }

    [Fact]
    public void M2_ConstructAndDispose_ParserAndModel()
    {
        // M2 parsing needs a VirtualPathFileSystem for the sibling
        // .skin / .skel / .anim files — that's Phase 4 work. For now
        // verify the handle types load + dispose cleanly.
        using var parser = new M2Parser();
        using var model = new M2Model();
        Assert.False(parser.IsInvalid);
        Assert.False(model.IsInvalid);
        Assert.False(parser.HasIssues);
        Assert.Empty(parser.Issues);
    }

    [Fact]
    public void M3_ParsesRealHeroesOfTheStormModel()
    {
        Assert.True(File.Exists(M3Fixture), $"fixture not found: {M3Fixture}");
        var m3Bytes = File.ReadAllBytes(M3Fixture);

        using var parser = new M3Parser();
        using var model = parser.ParseBuffer(m3Bytes);

        Assert.NotNull(model);
        Assert.False(model.IsInvalid);
        Assert.False(parser.HasIssues);
    }

    [Fact]
    public void Mdx_RoundTrip_To_Mdl_Text()
    {
        var mdxBytes = File.ReadAllBytes(MdxFixture);
        using var parser = new MdxParser();
        using var model = parser.ParseBufferFormat(mdxBytes, MDLXFormat.MDX)
                          ?? throw new InvalidDataException();
        using var writer = new MdxWriter();
        var mdlText = writer.WriteMdxFormatMdlFormat(model, MDLXFormat.MDL, MdlFormat.Hiveworkshop);
        Assert.NotNull(mdlText);
        Assert.True(mdlText.Length > 0);
        // MDL text starts with `Version {`.
        var text = System.Text.Encoding.UTF8.GetString(mdlText, 0, Math.Min(20, mdlText.Length));
        Assert.Contains("Version", text);
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
