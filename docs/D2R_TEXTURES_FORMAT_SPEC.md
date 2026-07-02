# Diablo II: Resurrected `.texture` Format Specification

Reverse-engineered from the 1,294 VFX texture files under
`Corpus/data/hd/vfx/textures/`. Every claim below was validated against the
full corpus; the format/colour mappings were confirmed by decoding the block
data with a BC1–BC7 decoder and inspecting the resulting images.

All multi-byte integers are **little-endian**.

---

## 1. Overview

`.texture` is Diablo II: Resurrected's GPU-ready texture container. Each file
holds a single 2-D texture with its full mip chain, stored as raw
D3D11-compatible surface data (uncompressed BGRA or a block-compressed BC
format). There is no compression of the container itself and no per-mip
header — the file is a fixed header, a mip directory, then the mip bytes
back-to-back.

```
+--------------------+
| Header   (0x24)    |
+--------------------+
| Mip directory      |   mipCount × 8 bytes
+--------------------+
| Mip 0 data         |
| Mip 1 data         |   contiguous, largest first
| ...                |
+--------------------+
```

---

## 2. Header (36 bytes, offset 0x00–0x23)

| Offset | Size | Type  | Field        | Notes |
|-------:|-----:|-------|--------------|-------|
| 0x00   | 4    | char  | `magic`      | `3C 44 45 28` — ASCII `"<DE("`. Constant. |
| 0x04   | 2    | u16   | `format`     | Pixel-format code. See §4. (High byte always 0.) |
| 0x06   | 1    | u8    | `type`       | Texture dimensionality. `2` = 2-D for every file in the corpus. |
| 0x07   | 1    | u8    | `mipCount`   | Number of mip levels. Always equal to the u32 at 0x1C. |
| 0x08   | 4    | u32   | `width`      | Width of mip 0, in pixels. |
| 0x0C   | 4    | u32   | `height`     | Height of mip 0, in pixels. |
| 0x10   | 4    | u32   | `arraySize`  | Array layers / depth. Always `1`. |
| 0x14   | 4    | u32   | `reserved0`  | Always `0`. |
| 0x18   | 4    | u32   | `reserved1`  | Always `0`. |
| 0x1C   | 4    | u32   | `mipCount`   | Number of mip levels (duplicate of byte 0x07, as a u32). |
| 0x20   | 4    | u32   | `reserved2`  | Always `4`. (Likely a format-class / alignment constant.) |

Observed ranges in the corpus: `width`/`height` ∈ 1…1024, `mipCount` ∈ 1…11.
277 files are single-level (`mipCount == 1`); the rest carry a full chain down
to 1×1. 9 files are non-power-of-two (e.g. 261×256, 600×600, 1024×116); these
are always uncompressed (`format == 31`).

---

## 3. Mip directory (offset 0x24)

Immediately after the header is an array of `mipCount` entries, 8 bytes each,
ordered largest mip first:

| Offset within entry | Size | Type | Field    |
|--------------------:|-----:|------|----------|
| +0x00               | 4    | u32  | `size`   | Byte length of this mip's surface data. |
| +0x04               | 4    | u32  | `offset` | **Self-relative** pointer to the mip data (see below). |

### 3.1 Self-relative offset

`offset` is measured **from the address of the `offset` field itself**, not
from the start of the file. For mip *i* (entry *i* starts at `0x24 + i*8`, so
its `offset` field sits at `0x28 + i*8`):

```
absolute_data_offset(i) = (0x28 + i*8) + offset_field(i)
```

Equivalently, because mips are stored contiguously right after the directory:

```
data_start          = 0x24 + mipCount*8
absolute_offset(0)  = data_start
absolute_offset(i)  = absolute_offset(i-1) + size(i-1)
```

Both formulas agree for all 1,294 files, and the sum of all mip `size` values
plus the header and directory exactly equals the file length (no padding, no
trailing data).

### 3.2 Minimum mip size

Each `size` is the true byte length of that level **except** that
block-compressed levels are clamped to one full 4×4 block: BC1/BC4 mips never
report fewer than 8 bytes, and BC3/BC5/BC7 mips never fewer than 16 bytes,
even for 2×2 and 1×1 levels.

### 3.3 Worked example — `common/t_fx_4x4_white.texture` (108 bytes)

```
00: 3C 44 45 28  3D 00  02 03   magic, format=0x3D(61), type=2, mipCount=3
08: 04 00 00 00              width  = 4
0C: 04 00 00 00              height = 4
10: 01 00 00 00              arraySize = 1
14: 00 00 00 00              reserved0
18: 00 00 00 00              reserved1
1C: 03 00 00 00              mipCount = 3
20: 04 00 00 00              reserved2
24: 10 00 00 00  14 00 00 00   mip0: size=16, off=20 -> 0x28+20 = 60 (0x3C)
2C: 10 00 00 00  1C 00 00 00   mip1: size=16, off=28 -> 0x30+28 = 76 (0x4C)
34: 10 00 00 00  24 00 00 00   mip2: size=16, off=36 -> 0x38+36 = 92 (0x5C)
3C: <16 bytes BC3 block>        mip0 (4×4)
4C: <16 bytes BC3 block>        mip1 (2×2, clamped to 1 block)
5C: <16 bytes BC3 block>        mip2 (1×1, clamped to 1 block)
```

---

## 4. Pixel formats

There is **no explicit BC-format byte** in the header. `reserved2` (0x20) is a
constant `4`, and `type` (0x06) only encodes dimensionality. The actual surface
format is given by the `format` code at **offset 0x04**:

| `format` | Block / pixel        | Bytes per 4×4 block | DXGI equivalent              | Typical use (by filename) |
|---------:|----------------------|--------------------:|------------------------------|---------------------------|
| 31       | Uncompressed BGRA8   | 64 (= 16 px × 4 B)  | `B8G8R8A8_UNORM`             | Colour gradients / ramps, alpha-remaps, distortion LUTs. Mostly single-mip. |
| 58       | BC1 (DXT1)           | 8                   | `BC1_UNORM` (sRGB albedo)   | `_alb` colour albedo. |
| 62       | BC3 (DXT5)           | 16                  | `BC3_UNORM` (sRGB albedo)   | `_alb` colour albedo (with alpha). |
| 61       | BC3 (DXT5 / DXT5nm)  | 16                  | `BC3_UNORM` (linear)        | `_nrm` normal maps, `_orm`, masks with alpha. |
| 63       | BC4 (single channel) | 8                   | `BC4_UNORM` (linear)        | `_mono`, `_mask`, `_orm`, grayscale data, single-channel albedo. |
| 65       | BC5 (two channel)    | 16                  | `BC5_UNORM` (linear)        | `_distortion` / flow maps (R = X, G = Y, B unused). |

Counts in the corpus: `31`×278, `58`×79, `61`×202, `62`×96, `63`×614, `65`×23.

### 4.1 How the formats were confirmed

* **Block size**: `mip0.size / blockCount` gives 8 B/block (BC1/BC4) or
  16 B/block (BC3/BC5/BC7) or 64 B/block (uncompressed 32-bpp).
* **BC sub-format**: every candidate decode was scored by image coherence
  (adjacent-pixel difference) over the full corpus. The winner was decisive
  and consistent per `format` code — e.g. BC4 beats BC1 on all 614 `format==63`
  files; BC5 wins on `format==65`; BC3 wins on `format==61`/`62`.
* **Channel semantics** were verified visually:
  * `format==65` decodes to a clean two-channel RG vector field on a B=0
    background — the signature of a BC5 distortion/flow map.
  * `format==61` normal maps decode as **DXT5nm**: the BC3 colour block pins
    R=255, B=0 and stores normal **Y in green**, while normal **X is in the
    alpha** channel (both averaging ~127 over a flat surface). Z is meant to be
    reconstructed as `sqrt(1 − x² − y²)` at sample time.

### 4.2 Colour space

sRGB vs. linear is **not stored** in the file; it follows the texture's role.
By convention `_alb` (albedo) surfaces are sRGB and everything else
(normals, ORM, masks, distortion, gradients used as LUTs) is linear. The two
albedo formats (`58` = BC1, `62` = BC3) differ from their data-channel
counterparts (`63` = BC4, `61` = BC3) only in this intended usage.

### 4.3 BGRA byte order (`format == 31`)

Uncompressed surfaces are stored in **B, G, R, A** byte order
(`B8G8R8A8_UNORM`), matching D3D11's default. Decoding as straight RGBA
produces swapped red/blue.

---

## 5. Reference parser (pseudocode)

```python
import struct

def parse_texture(data: bytes):
    assert data[:4] == b"<DE("
    fmt        = struct.unpack_from("<H", data, 0x04)[0]
    tex_type   = data[0x06]
    width, height = struct.unpack_from("<II", data, 0x08)
    array_size = struct.unpack_from("<I", data, 0x10)[0]
    mip_count  = struct.unpack_from("<I", data, 0x1C)[0]

    mips = []
    for i in range(mip_count):
        entry = 0x24 + i * 8
        size, rel = struct.unpack_from("<II", data, entry)
        abs_off = (entry + 4) + rel          # self-relative pointer
        mips.append(data[abs_off:abs_off + size])

    return fmt, width, height, array_size, mips

BLOCK_FORMAT = {            # format code -> (decoder, block bytes)
    31: ("BGRA8", 64),
    58: ("BC1",    8),
    61: ("BC3",   16),      # DXT5nm when used for normals
    62: ("BC3",   16),
    63: ("BC4",    8),
    65: ("BC5",   16),
}
```

To decode a mip's pixels: for `format == 31`, the surface is
`width * height * 4` BGRA bytes; otherwise feed the mip bytes to the matching
BC decoder at that level's dimensions (`max(1, width >> level)` etc.).

---

## 6. Open questions / notes

* `reserved2` (0x20) is always `4` and `type` (0x06) always `2`; their full
  meaning (cube/volume/array variants) cannot be inferred from this 2-D-only
  corpus.
* The `format` field is labelled "format" rather than "version" because it
  predicts the surface format exactly; the specific integer values
  (31/58/61/62/63/65) look like entries in an internal Blizzard texture-format
  enum rather than a sequential file-version counter.
* sRGB/linear is a convention, not a stored flag (§4.2).
* Filename suffixes are a reliable hint to channel intent but are not part of
  the binary format: `_alb` (albedo), `_nrm` (normal), `_orm`
  (occlusion/roughness/metallic), `_mask`/`_mono` (single channel),
  `_distortion` (flow), `_gradient`/`_grad` (LUT ramps).
