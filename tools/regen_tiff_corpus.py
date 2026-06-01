# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
"""Regenerate Corpus/TIFF/ref/ fixtures + .golden.json sidecars.

The fixtures drive the TDD loop for the TIFF parser. Each fixture pairs a
.tif file with a hand-checked .golden.json describing expected width/height,
pixel format, sRGB flag, and a few spot-check pixel values. Re-running this
script regenerates the bytes deterministically; if the format spec we follow
changes intentionally, the golden values get regenerated together with the
bytes so tests stay coherent.

Pattern (32x32):
    Top-left  (x<16, y<16): RGBA (255,   0,   0, 255)
    Top-right (x>=16,y<16): RGBA (  0, 255,   0, 128)
    Bot-left  (x<16,y>=16): RGBA (  0,   0, 255, 255)
    Bot-right (x>=16,y>=16): RGBA (x*8, y*8, (x+y)*4 & 0xFF, 255)
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

from PIL import Image, TiffImagePlugin

REF_DIR = Path(__file__).resolve().parent.parent / "Corpus" / "TIFF" / "ref"
W, H = 32, 32


# =============================================================================
# Pattern generation
# =============================================================================

def build_rgba8_pattern() -> bytes:
    out = bytearray(W * H * 4)
    for y in range(H):
        for x in range(W):
            if x < 16 and y < 16:
                r, g, b, a = 255, 0, 0, 255
            elif x >= 16 and y < 16:
                r, g, b, a = 0, 255, 0, 128
            elif x < 16 and y >= 16:
                r, g, b, a = 0, 0, 255, 255
            else:
                r, g, b, a = (x * 8) & 0xFF, (y * 8) & 0xFF, ((x + y) * 4) & 0xFF, 255
            o = (y * W + x) * 4
            out[o] = r
            out[o + 1] = g
            out[o + 2] = b
            out[o + 3] = a
    return bytes(out)


def build_rgb8_pattern() -> bytes:
    rgba = build_rgba8_pattern()
    out = bytearray(W * H * 3)
    for i in range(W * H):
        out[i * 3:i * 3 + 3] = rgba[i * 4:i * 4 + 3]
    return bytes(out)


def build_gray8_pattern() -> bytes:
    rgba = build_rgba8_pattern()
    out = bytearray(W * H)
    for i in range(W * H):
        r, g, b = rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]
        out[i] = (r * 299 + g * 587 + b * 114 + 500) // 1000
    return bytes(out)


# Spot-check coordinates used in every .golden.json. The TIFF parser must
# match all of these to be considered green.
SPOT_COORDS = [(0, 0), (24, 0), (0, 24), (24, 24), (15, 15), (16, 16)]


def spot_rgba8(data: bytes) -> list[list[int]]:
    out = []
    for x, y in SPOT_COORDS:
        o = (y * W + x) * 4
        out.append([data[o], data[o + 1], data[o + 2], data[o + 3]])
    return out


def spot_rgb8(data: bytes) -> list[list[int]]:
    out = []
    for x, y in SPOT_COORDS:
        o = (y * W + x) * 3
        out.append([data[o], data[o + 1], data[o + 2]])
    return out


def spot_gray8(data: bytes) -> list[list[int]]:
    return [[data[y * W + x]] for x, y in SPOT_COORDS]


# =============================================================================
# Golden JSON writer
# =============================================================================

def write_golden(path: Path, *, fmt: str, channels: int,
                 srgb: bool, spots: list[list[int]],
                 notes: str = "") -> None:
    """Write a .golden.json sidecar next to a fixture.

    The schema is intentionally tiny:
        width, height — pixel dimensions
        format        — PixelFormat enum name expected after parsing
        srgb          — expected isSrgb() value
        spots         — list of [x, y, [c0, c1, ...]] for each SPOT_COORD;
                        the channel layout matches the source channel count
                        BEFORE the parser lifts to RGBA8 (so a gray fixture
                        has 1-element spots, a RGB has 3, RGBA has 4).
        notes         — optional human-readable note
    """
    pixels = [[x, y, s] for (x, y), s in zip(SPOT_COORDS, spots)]
    obj = {
        "width": W,
        "height": H,
        "format": fmt,
        "channels": channels,
        "srgb": srgb,
        "spots": pixels,
    }
    if notes:
        obj["notes"] = notes
    path.write_text(json.dumps(obj, indent=2) + "\n")


# =============================================================================
# PIL-driven fixture emission (little-endian)
# =============================================================================

def emit_pil_uncompressed_rgb_le() -> Path:
    img = Image.frombytes("RGB", (W, H), build_rgb8_pattern())
    path = REF_DIR / "uncompressed_rgb_le_32.tif"
    img.save(path, compression="raw")
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=3, srgb=False,
                 spots=spot_rgb8(build_rgb8_pattern()),
                 notes="Photometric=RGB, Compression=None, II byte order.")
    return path


def emit_pil_uncompressed_rgba_le() -> Path:
    img = Image.frombytes("RGBA", (W, H), build_rgba8_pattern())
    path = REF_DIR / "uncompressed_rgba_le_32.tif"
    img.save(path, compression="raw")
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=4, srgb=False,
                 spots=spot_rgba8(build_rgba8_pattern()),
                 notes="Photometric=RGB, SamplesPerPixel=4, "
                       "ExtraSamples=2 (unassociated alpha), II byte order.")
    return path


def emit_pil_uncompressed_gray_le() -> Path:
    img = Image.frombytes("L", (W, H), build_gray8_pattern())
    path = REF_DIR / "uncompressed_gray_le_32.tif"
    img.save(path, compression="raw")
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=1, srgb=False,
                 spots=spot_gray8(build_gray8_pattern()),
                 notes="Photometric=BlackIsZero, SamplesPerPixel=1, "
                       "II byte order. Parser lifts to RGBA8.")
    return path


def emit_pil_packbits_rgb_le() -> Path:
    img = Image.frombytes("RGB", (W, H), build_rgb8_pattern())
    path = REF_DIR / "packbits_rgb_le_32.tif"
    img.save(path, compression="packbits")
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=3, srgb=False,
                 spots=spot_rgb8(build_rgb8_pattern()),
                 notes="Compression=32773 (PackBits), II byte order.")
    return path


def emit_pil_deflate_rgb_le() -> Path:
    img = Image.frombytes("RGB", (W, H), build_rgb8_pattern())
    path = REF_DIR / "deflate_rgb_le_32.tif"
    img.save(path, compression="tiff_deflate")
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=3, srgb=False,
                 spots=spot_rgb8(build_rgb8_pattern()),
                 notes="Compression=32946 / 8 (Deflate), II byte order.")
    return path


def emit_pil_lzw_rgb_le() -> Path:
    img = Image.frombytes("RGB", (W, H), build_rgb8_pattern())
    path = REF_DIR / "lzw_rgb_le_32.tif"
    img.save(path, compression="tiff_lzw")
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=3, srgb=False,
                 spots=spot_rgb8(build_rgb8_pattern()),
                 notes="Compression=5 (LZW with TIFF early-change), "
                       "II byte order.")
    return path


def emit_pil_lzw_predictor2_rgb_le() -> Path:
    img = Image.frombytes("RGB", (W, H), build_rgb8_pattern())
    path = REF_DIR / "lzw_predictor2_rgb_le_32.tif"
    img.save(path, compression="tiff_lzw", tiffinfo={317: 2})
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=3, srgb=False,
                 spots=spot_rgb8(build_rgb8_pattern()),
                 notes="Compression=5 LZW, Predictor=2 (horizontal differencing), "
                       "II byte order.")
    return path


# ---------------------------------------------------------------------------
# 16-bit fixtures (Phase 3)
# ---------------------------------------------------------------------------

def build_rgb16_pattern() -> bytes:
    """16-bit-per-sample RGB. Values are the 8-bit pattern << 8 plus the
    8-bit pattern (i.e. 0xAA -> 0xAAAA), so spot-checking is easy."""
    rgba = build_rgba8_pattern()
    out = bytearray(W * H * 3 * 2)
    for i in range(W * H):
        r, g, b = rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]
        # store little-endian 16-bit
        for j, v in enumerate((r, g, b)):
            v16 = (v << 8) | v
            out[(i * 3 + j) * 2] = v16 & 0xFF
            out[(i * 3 + j) * 2 + 1] = (v16 >> 8) & 0xFF
    return bytes(out)


def spot_rgb16(data: bytes) -> list[list[int]]:
    out = []
    for x, y in SPOT_COORDS:
        o = (y * W + x) * 3 * 2
        r = data[o] | (data[o + 1] << 8)
        g = data[o + 2] | (data[o + 3] << 8)
        b = data[o + 4] | (data[o + 5] << 8)
        out.append([r, g, b])
    return out


def emit_pil_uncompressed_rgb16_le() -> Path:
    img = Image.frombytes("RGB;16N", (W, H), build_rgb16_pattern())
    if img.mode != "RGB":
        # Fall back: write raw u16 RGB by writing via PIL's I;16 mode trickery
        # is fragile across PIL versions; use the raw image saver path.
        from PIL import TiffImagePlugin  # noqa
        img = Image.new("RGB", (W, H))
        img.putdata([(r, g, b) for r, g, b in
                     [(255, 0, 0), (0, 255, 0), (0, 0, 255), (192, 192, 192)]
                     * (W * H // 4)])
    path = REF_DIR / "uncompressed_rgb16_le_32.tif"
    img.save(path, compression="raw")
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA16", channels=3, srgb=False,
                 spots=spot_rgb16(build_rgb16_pattern()),
                 notes="16-bit RGB uncompressed, II.")
    return path


# ---------------------------------------------------------------------------
# Palette fixture (Phase 3)
# ---------------------------------------------------------------------------

def emit_pil_palette_8bit_le() -> Path:
    """8-bit paletted image. Index values get looked up in ColorMap (3*256 u16
    entries) to produce RGB. We construct a 4-color palette and assign the
    four quadrants of the pattern to indices 0..3, leaving the rest unused."""
    indices = bytearray(W * H)
    for y in range(H):
        for x in range(W):
            if x < 16 and y < 16:
                indices[y * W + x] = 0
            elif x >= 16 and y < 16:
                indices[y * W + x] = 1
            elif x < 16 and y >= 16:
                indices[y * W + x] = 2
            else:
                indices[y * W + x] = 3
    img = Image.frombytes("P", (W, H), bytes(indices))
    # PIL expects palette in interleaved R0,G0,B0, R1,G1,B1, ... order (768 bytes
    # for 256 entries). TIFF on-disk format is separated planes (R*256, G*256,
    # B*256 u16) — PIL converts on save.
    pal8 = bytearray(768)
    colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (192, 192, 192)]
    for i, (r, g, b) in enumerate(colors):
        pal8[i * 3:i * 3 + 3] = bytes([r, g, b])
    img.putpalette(bytes(pal8))
    path = REF_DIR / "palette_8bit_le_32.tif"
    img.save(path, compression="raw")
    spots = []
    for x, y in SPOT_COORDS:
        idx = indices[y * W + x]
        spots.append(list(colors[idx]))
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=3, srgb=False,
                 spots=spots,
                 notes="8-bit palette, 4 colors. Lift to RGBA8.")
    return path


def emit_pil_deflate_predictor2_rgb_le() -> Path:
    img = Image.frombytes("RGB", (W, H), build_rgb8_pattern())
    path = REF_DIR / "deflate_predictor2_rgb_le_32.tif"
    img.save(path, compression="tiff_deflate", tiffinfo={317: 2})
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=3, srgb=False,
                 spots=spot_rgb8(build_rgb8_pattern()),
                 notes="Compression=8 Deflate, Predictor=2, II byte order.")
    return path


# =============================================================================
# Big-endian fixture emission via post-process byte-swap
# =============================================================================

# Per TIFF 6.0 Section 2, the byte order header determines the layout of
# every multi-byte field in the file: header, IFD entry count, each IFD
# entry's 12 bytes (tag/type/count/value-or-offset), the next-IFD offset,
# and any tag values stored at an offset rather than inline. The pixel
# payload bytes are NOT byte-swapped for 8-bit samples — bytes are bytes.
# For 16-bit samples the per-sample u16s ARE swapped (we'll add that path
# when we introduce 16-bit fixtures).
#
# This converter walks the LE TIFF, swaps every multi-byte field, and
# emits the equivalent MM-ordered file.

# Bytes per FieldType (TIFF 6.0 Table 2).
_TYPE_BYTES = {
    1: 1,   # BYTE
    2: 1,   # ASCII
    3: 2,   # SHORT
    4: 4,   # LONG
    5: 8,   # RATIONAL (two LONGs)
    6: 1,   # SBYTE
    7: 1,   # UNDEFINED
    8: 2,   # SSHORT
    9: 4,   # SLONG
    10: 8,  # SRATIONAL
    11: 4,  # FLOAT
    12: 8,  # DOUBLE
}


def _swap_u16(b: bytes) -> bytes:
    return bytes(reversed(b))


def _swap_u32(b: bytes) -> bytes:
    return bytes(reversed(b))


def _swap_value_or_offset(b: bytes, field_type: int, count: int,
                          element_bytes: int) -> bytes:
    """Byte-swap the 4-byte value-or-offset inline payload.

    If the values fit inline (total <= 4 bytes), swap each element in place.
    Otherwise it's an offset — swap as a u32.
    """
    total = count * element_bytes
    if total > 4:
        return _swap_u32(b)
    out = bytearray(b)
    for i in range(count):
        s, e = i * element_bytes, (i + 1) * element_bytes
        if element_bytes == 2:
            out[s:e] = _swap_u16(bytes(out[s:e]))
        elif element_bytes == 4:
            out[s:e] = _swap_u32(bytes(out[s:e]))
        # 1-byte types: no swap needed
    return bytes(out)


def le_to_be_tiff(le_bytes: bytes) -> bytes:
    """Convert a little-endian classic TIFF into its big-endian equivalent.

    Walks one IFD (sufficient for our corpus). 8-bit-sample pixel payload
    bytes are passed through untouched.
    """
    assert le_bytes[:2] == b"II", "expected 'II' magic"
    assert struct.unpack("<H", le_bytes[2:4])[0] == 42
    ifd_offset = struct.unpack("<I", le_bytes[4:8])[0]

    out = bytearray(le_bytes)
    # Header: byte order MM, magic 42 BE, ifd_offset BE.
    out[0:2] = b"MM"
    out[2:4] = struct.pack(">H", 42)
    out[4:8] = struct.pack(">I", ifd_offset)

    num_entries = struct.unpack("<H", le_bytes[ifd_offset:ifd_offset + 2])[0]
    out[ifd_offset:ifd_offset + 2] = struct.pack(">H", num_entries)

    entry_base = ifd_offset + 2

    # Records of (offset_in_buffer, element_size_bytes) for tag-value payloads
    # stored at an offset rather than inline. We byte-swap those after walking.
    offset_payloads: list[tuple[int, int, int]] = []  # (offset, count, elem_bytes)

    for i in range(num_entries):
        eo = entry_base + i * 12
        tag = struct.unpack("<H", le_bytes[eo:eo + 2])[0]
        ftype = struct.unpack("<H", le_bytes[eo + 2:eo + 4])[0]
        count = struct.unpack("<I", le_bytes[eo + 4:eo + 8])[0]
        value_bytes = le_bytes[eo + 8:eo + 12]
        elem_bytes = _TYPE_BYTES.get(ftype, 1)
        total = count * elem_bytes

        # tag (u16), type (u16) -> swap
        out[eo:eo + 2] = struct.pack(">H", tag)
        out[eo + 2:eo + 4] = struct.pack(">H", ftype)
        # count (u32) -> swap
        out[eo + 4:eo + 8] = struct.pack(">I", count)
        # value-or-offset: if total > 4 it's a u32 offset; otherwise inline data.
        if total > 4:
            offset = struct.unpack("<I", value_bytes)[0]
            out[eo + 8:eo + 12] = struct.pack(">I", offset)
            if elem_bytes in (2, 4):
                offset_payloads.append((offset, count, elem_bytes))
        else:
            out[eo + 8:eo + 12] = _swap_value_or_offset(
                value_bytes, ftype, count, elem_bytes)

    # next-IFD offset (u32) — 0 for single-IFD files
    next_off = entry_base + num_entries * 12
    nv = struct.unpack("<I", le_bytes[next_off:next_off + 4])[0]
    out[next_off:next_off + 4] = struct.pack(">I", nv)

    # Swap each offset-stored tag payload in place.
    for offset, count, elem in offset_payloads:
        for i in range(count):
            s = offset + i * elem
            e = s + elem
            out[s:e] = bytes(reversed(out[s:e]))

    return bytes(out)


def emit_be_variant(le_path: Path, suffix: str = "_be") -> Path:
    le_bytes = le_path.read_bytes()
    be_bytes = le_to_be_tiff(le_bytes)
    new_name = le_path.stem.replace("_le_", "_be_") + le_path.suffix
    be_path = le_path.parent / new_name
    be_path.write_bytes(be_bytes)
    # Copy golden JSON — pixel values are byte-order-independent for 8-bit.
    le_golden = le_path.with_suffix(".golden.json")
    if le_golden.exists():
        obj = json.loads(le_golden.read_text())
        obj["notes"] = (obj.get("notes", "") + " (MM byte order)").strip()
        be_path.with_suffix(".golden.json").write_text(json.dumps(obj, indent=2) + "\n")
    return be_path


# =============================================================================
# Hand-crafted diagnostic fixtures
# =============================================================================

def emit_multistrip_rgb_le(rows_per_strip: int = 8) -> Path:
    """Hand-crafted multi-strip uncompressed RGB LE TIFF.

    Pixel layout is contiguous RGB; we just record several (offset, byteCount)
    pairs in the IFD instead of one. Strips of `rows_per_strip` rows each.
    """
    assert H % rows_per_strip == 0, "rows_per_strip must divide image height"
    num_strips = H // rows_per_strip
    pixels = build_rgb8_pattern()
    bytes_per_strip = rows_per_strip * W * 3

    # IFD with 9 entries: Width, Length, BitsPerSample, Compression,
    # Photometric, StripOffsets, SamplesPerPixel, RowsPerStrip, StripByteCounts.
    # IFD entries are 12 bytes each; entry count is 2 bytes; next-IFD u32 = 0.

    NUM_ENTRIES = 9
    # Layout: header (8) + IFD (2 + 12*N + 4) + payload areas.
    ifd_size = 2 + NUM_ENTRIES * 12 + 4
    header_off = 0
    ifd_off = 8
    # Inline-or-offset rule for tag values:
    #   - BitsPerSample (3 SHORTs = 6 bytes)         => offset payload
    #   - StripOffsets / StripByteCounts (N LONGs)   => offset payload
    # Everything else (single SHORT/LONG) fits inline.

    after_ifd = ifd_off + ifd_size
    bps_off = after_ifd                           # 3 SHORTs => 6 bytes
    soff_off = bps_off + 6                        # N LONGs  => 4*N bytes
    sbc_off = soff_off + 4 * num_strips           # N LONGs
    pixels_off = sbc_off + 4 * num_strips
    # Compute strip offsets within `pixels_off` block.
    strip_offsets = [pixels_off + i * bytes_per_strip for i in range(num_strips)]
    strip_byte_counts = [bytes_per_strip] * num_strips

    def s16(v: int) -> bytes:
        return struct.pack("<H", v)

    def s32(v: int) -> bytes:
        return struct.pack("<I", v)

    def ifd_entry(tag: int, ftype: int, count: int, value_bytes: bytes) -> bytes:
        assert len(value_bytes) == 4
        return s16(tag) + s16(ftype) + s32(count) + value_bytes

    # value_bytes encoding helper for an inline SHORT (count=1).
    def short_inline(v: int) -> bytes:
        return s16(v) + b"\x00\x00"

    # value_bytes for an inline LONG (count=1).
    def long_inline(v: int) -> bytes:
        return s32(v)

    out = bytearray()
    # Header
    out += b"II" + s16(42) + s32(ifd_off)
    # IFD: entry count then sorted-by-tag entries.
    out += s16(NUM_ENTRIES)
    out += ifd_entry(256, 4, 1, long_inline(W))                # ImageWidth (LONG)
    out += ifd_entry(257, 4, 1, long_inline(H))                # ImageLength (LONG)
    out += ifd_entry(258, 3, 3, s32(bps_off))                  # BitsPerSample (3x SHORT)
    out += ifd_entry(259, 3, 1, short_inline(1))               # Compression=None
    out += ifd_entry(262, 3, 1, short_inline(2))               # Photometric=RGB
    out += ifd_entry(273, 4, num_strips, s32(soff_off))        # StripOffsets
    out += ifd_entry(277, 3, 1, short_inline(3))               # SamplesPerPixel=3
    out += ifd_entry(278, 4, 1, long_inline(rows_per_strip))   # RowsPerStrip
    out += ifd_entry(279, 4, num_strips, s32(sbc_off))         # StripByteCounts
    out += s32(0)                                              # next-IFD = end

    assert len(out) == after_ifd
    # BitsPerSample payload (3 SHORTs = 8,8,8)
    out += s16(8) + s16(8) + s16(8)
    # StripOffsets payload (N LONGs)
    for o in strip_offsets:
        out += s32(o)
    # StripByteCounts payload (N LONGs)
    for c in strip_byte_counts:
        out += s32(c)
    assert len(out) == pixels_off
    # Pixel data.
    out += pixels

    path = REF_DIR / f"multistrip_rgb_le_32_rps{rows_per_strip}.tif"
    path.write_bytes(bytes(out))
    write_golden(path.with_suffix(".golden.json"),
                 fmt="RGBA8", channels=3, srgb=False,
                 spots=spot_rgb8(build_rgb8_pattern()),
                 notes=f"Hand-crafted multi-strip RGB LE, "
                       f"{num_strips} strips of {rows_per_strip} rows each.")
    return path


def emit_bigtiff_header_only() -> Path:
    """A 16-byte BigTIFF header (no IFD). Parser must reject with 'BigTIFF' diag."""
    # II magic, 43, 8 (always), 0, then IFD offset (u64).
    data = b"II" + struct.pack("<H", 43) + struct.pack("<HHQ", 8, 0, 16)
    path = REF_DIR / "bigtiff_header.tif"
    path.write_bytes(data)
    # No golden JSON — this is a rejection test.
    return path


def emit_truncated_header() -> Path:
    """Only 4 bytes — smaller than even a classic TIFF header. Reject cleanly."""
    path = REF_DIR / "truncated_header.tif"
    path.write_bytes(b"II*\x00")
    return path


# =============================================================================
# Driver
# =============================================================================

def main() -> int:
    REF_DIR.mkdir(parents=True, exist_ok=True)

    le_paths = [
        emit_pil_uncompressed_rgb_le(),
        emit_pil_uncompressed_rgba_le(),
        emit_pil_uncompressed_gray_le(),
        emit_pil_packbits_rgb_le(),
        emit_pil_deflate_rgb_le(),
        emit_pil_lzw_rgb_le(),
        emit_pil_lzw_predictor2_rgb_le(),
        emit_pil_deflate_predictor2_rgb_le(),
        emit_pil_palette_8bit_le(),
    ]
    for le in le_paths:
        emit_be_variant(le)

    emit_multistrip_rgb_le(rows_per_strip=8)
    emit_bigtiff_header_only()
    emit_truncated_header()

    print(f"wrote {len(list(REF_DIR.glob('*.tif')))} fixtures to {REF_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
