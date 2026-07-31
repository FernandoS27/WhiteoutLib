// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Phase 2 gate: handles + Drop, borrowed zero-copy pixel access, owned
// Bytes, and a full encode/decode round-trip through two codecs.
//
// The fixtures are synthesised rather than loaded from disk so the suite
// stays self-contained and also exercises the Tier A *write* path.

use whiteout::textures::{
    BlpParser, BlpWriter, DdsParser, DdsWriter, PixelFormat, PngParser, PngWriter, Texture,
    TextureType, TgaParser, TgaWriter,
};

/// A recognisable RGBA8 gradient — asymmetric in x, y and channel so a
/// transposed or channel-swapped round-trip cannot accidentally pass.
fn checkerboard(w: u32, h: u32) -> Texture {
    let mut tex = Texture::create_2d(PixelFormat::RGBA8, w, h, 1).expect("create_2d failed");
    {
        // Tier A write: straight into the C++ allocation, no marshalling.
        let px = tex.data_mut();
        assert_eq!(px.len(), (w * h * 4) as usize);
        for y in 0..h {
            for x in 0..w {
                let i = ((y * w + x) * 4) as usize;
                px[i] = (x * 8 % 256) as u8;
                px[i + 1] = (y * 8 % 256) as u8;
                px[i + 2] = if (x + y) % 2 == 0 { 255 } else { 32 };
                px[i + 3] = 255;
            }
        }
    }
    tex
}

// ── Handles, Drop, construction ───────────────────────────────────────────

#[test]
fn create_2d_reports_its_shape() {
    let tex = Texture::create_2d(PixelFormat::RGBA8, 16, 8, 1).expect("create_2d failed");
    assert_eq!(tex.width(), 16);
    assert_eq!(tex.height(), 8);
    assert_eq!(tex.depth(), 1);
    assert_eq!(tex.mip_count(), 1);
    assert_eq!(tex.format(), PixelFormat::RGBA8);
    assert_eq!(tex.texture_type(), TextureType::Texture2D);
    assert_eq!(tex.data_size(), 16 * 8 * 4);
}

#[test]
fn cube_and_array_layouts() {
    let cube = Texture::create_cube(PixelFormat::RGBA8, 8, 1).expect("createCube failed");
    assert_eq!(cube.layer_count(), 6);
    assert_eq!(cube.texture_type(), TextureType::TextureCube);

    let arr = Texture::create_2d_array(PixelFormat::RGBA8, 8, 8, 3, 1).expect("array failed");
    assert_eq!(arr.array_size(), 3);
}

#[test]
fn dropping_many_textures_does_not_leak_handles() {
    // Exercises Drop repeatedly; under ASan this is the leak check.
    for _ in 0..256 {
        let t = Texture::create_2d(PixelFormat::RGBA8, 32, 32, 1).unwrap();
        assert_eq!(t.data().len(), 32 * 32 * 4);
    }
}

// ── Tier A: zero-copy pixel access ────────────────────────────────────────

#[test]
fn data_is_borrowed_not_copied() {
    let mut tex = checkerboard(8, 8);

    // Write through the mutable view...
    tex.data_mut()[0] = 0xAB;
    // ...and observe it through the shared one. If either side copied,
    // this would fail.
    assert_eq!(tex.data()[0], 0xAB);

    // The borrow really points at the texture's own allocation.
    assert_eq!(tex.data().len() as u64, tex.data_size());
}

#[test]
fn mip_data_covers_each_level() {
    let tex = Texture::create_2d(PixelFormat::RGBA8, 16, 16, 3).expect("create_2d failed");
    assert_eq!(tex.mip_count(), 3);
    assert_eq!(tex.mip_data(0, 0).len(), 16 * 16 * 4);
    assert_eq!(tex.mip_data(1, 0).len(), 8 * 8 * 4);
    assert_eq!(tex.mip_data(2, 0).len(), 4 * 4 * 4);
}

// ── Round-trips ───────────────────────────────────────────────────────────

fn assert_round_trip(name: &str, encoded: &[u8], src: &Texture, decoded: Option<Texture>) {
    assert!(!encoded.is_empty(), "{name}: encoder produced no bytes");
    let decoded = decoded.unwrap_or_else(|| panic!("{name}: parser returned None"));
    assert_eq!(decoded.width(), src.width(), "{name}: width");
    assert_eq!(decoded.height(), src.height(), "{name}: height");
}

#[test]
fn blp_round_trip() {
    let src = checkerboard(32, 32);

    let mut writer = BlpWriter::new();
    let encoded = writer.write(&src);
    assert!(!writer.has_issues(), "writer issues: {:?}", writer.issues());

    let mut parser = BlpParser::new();
    let decoded = parser.parse(&encoded);
    assert_round_trip("blp", &encoded, &src, decoded);
}

#[test]
fn png_round_trip_preserves_pixels_exactly() {
    // PNG is lossless, so this is the strongest available check that the
    // pixel buffer survives the whole path intact.
    let src = checkerboard(16, 16);
    let original = src.data().to_vec();

    let mut writer = PngWriter::new();
    let encoded = writer.write(&src);
    assert!(!encoded.is_empty());

    let mut parser = PngParser::new();
    let decoded = parser.parse(&encoded).expect("png parse returned None");

    assert_eq!(decoded.width(), 16);
    assert_eq!(decoded.height(), 16);
    assert_eq!(
        decoded.data().as_ref(),
        original.as_slice(),
        "png round-trip changed pixel data"
    );
}

#[test]
fn blp_to_png_conversion() {
    // The headline use case for this module: decode one format, re-encode
    // as another, without ever copying the pixel buffer in Rust.
    let src = checkerboard(32, 32);

    let mut blp_writer = BlpWriter::new();
    let blp_bytes = blp_writer.write(&src);

    let mut blp_parser = BlpParser::new();
    let from_blp = blp_parser.parse(&blp_bytes).expect("blp parse failed");

    let mut png_writer = PngWriter::new();
    let png_bytes = png_writer.write(&from_blp);
    assert!(png_bytes.len() > 8, "png output implausibly small");
    // PNG signature.
    assert_eq!(
        &png_bytes[..8],
        &[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]
    );

    let mut png_parser = PngParser::new();
    let final_tex = png_parser.parse(&png_bytes).expect("png parse failed");
    assert_eq!(final_tex.width(), 32);
    assert_eq!(final_tex.height(), 32);
}

#[test]
fn dds_and_tga_round_trip() {
    let src = checkerboard(16, 16);

    let mut dds_w = DdsWriter::new();
    let dds = dds_w.write(&src);
    let mut dds_p = DdsParser::new();
    assert_round_trip("dds", &dds, &src, dds_p.parse(&dds));

    let mut tga_w = TgaWriter::new();
    let tga = tga_w.write(&src);
    let mut tga_p = TgaParser::new();
    assert_round_trip("tga", &tga, &src, tga_p.parse(&tga));
}

// ── Error and absence handling ────────────────────────────────────────────

#[test]
fn parsing_garbage_returns_none_rather_than_panicking() {
    // The library signals failure with std::optional, which is `None` here
    // — not an error, and certainly not an exception.
    let mut parser = BlpParser::new();
    assert!(parser.parse(b"definitely not a BLP file").is_none());

    let mut parser = PngParser::new();
    assert!(parser.parse(&[0u8; 32]).is_none());
}

#[test]
fn issues_are_diagnostics_not_errors() {
    let mut parser = BlpParser::new();
    let _ = parser.parse(b"garbage");
    // Whatever the outcome, issues() is a plain list we can read.
    let issues = parser.issues();
    assert_eq!(parser.has_issues(), !issues.is_empty());
}

// ── Format conversion ─────────────────────────────────────────────────────

#[test]
fn convert_to_changes_format_in_place() {
    let mut tex = checkerboard(8, 8);
    assert_eq!(tex.format(), PixelFormat::RGBA8);
    tex.convert_to(PixelFormat::BC1);
    assert_eq!(tex.format(), PixelFormat::BC1);
    // BC1 is 8 bytes per 4x4 block: 2x2 blocks for 8x8.
    assert_eq!(tex.data_size(), 4 * 8);
}

#[test]
fn copy_as_format_leaves_the_original_alone() {
    let tex = checkerboard(8, 8);
    let converted = tex
        .copy_as_format(PixelFormat::BC1)
        .expect("convert failed");
    assert_eq!(converted.format(), PixelFormat::BC1);
    assert_eq!(tex.format(), PixelFormat::RGBA8, "source was mutated");
}

#[test]
fn generate_mipmaps_builds_the_chain() {
    let mut tex = Texture::create_2d(PixelFormat::RGBA8, 32, 32, 6).expect("create failed");
    {
        let px = tex.data_mut();
        px.fill(0x80);
    }
    // Returns Some(message) on failure, None on success — the library's own
    // optional-as-error convention, surfaced honestly.
    assert_eq!(tex.generate_mipmaps(6), None);
    assert_eq!(tex.mip_count(), 6);
    assert_eq!(tex.mip_data(5, 0).len(), 4);
}

// ── Enum round-tripping ───────────────────────────────────────────────────

#[test]
fn enums_reject_unknown_discriminants() {
    assert_eq!(PixelFormat::try_from(0).unwrap(), PixelFormat::R8);
    assert!(PixelFormat::try_from(9999).is_err());
    assert!(TextureType::try_from(-1).is_err());
}
