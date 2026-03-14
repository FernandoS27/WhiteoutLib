# WhiteoutLib — Architecture Guide for LLM Assistants

> This document is the authoritative map for navigating WhiteoutLib.
> Read it before making changes. It is optimized for LLM context windows.

## 1. Project Identity

WhiteoutLib is a **C++20 library** (public headers are C++11-compatible) for
reading and writing binary 3D model, texture, and data formats used in
Blizzard Entertainment games. It is a **standalone static library** with one
optional dependency (CascLib, fetched via CMake FetchContent).

| Area | Games | Formats |
|------|-------|---------|
| 3D models | Warcraft III | `.mdx` (Classic + Reforged) |
| 3D models | World of Warcraft | `.m2`, `.skin`, `.anim`, `.skel`, `.bone` |
| 3D models | StarCraft II / HotS | `.m3`, `.m3a` |
| Textures | WC3 / WoW | `.blp` (BLP1 + BLP2) |
| Textures | General | `.dds` (legacy + DX10) |
| Textures | General | `.bmp` (24/32-bit uncompressed) |
| Textures | General | `.tga` (uncompressed + RLE) |
| Textures | D3 / D4 | `.tex` (Blizzard proprietary) |
| Data files | Diablo III | SNO files (`.acr`, `.app`, `.ani`, etc.) |
| Data files | Diablo IV | SNO files (same extensions, different layout) |
| Archive access | D3 / D4 / WoW | CASC (via CascLib wrapper) |

**License:** BSD-3-Clause. **Build:** CMake ≥ 3.15, MSVC or GCC/Clang.

---

## 2. Repository Layout

```
WhiteoutLib/
├── CMakeLists.txt              # Single build file: library + all examples
├── include/whiteout/           # PUBLIC HEADERS (C++11-compatible surface)
│   ├── common_types.h          #   u8/u16/u32/i32/f32/f16/snorm/unorm/fixed_point
│   ├── compatibility.h         #   std::optional/std::span shims for C++11/14
│   ├── vector_types.h          #   Vector2f/3f/4f, Quaternion, Matrix44f
│   ├── utils.h                 #   VirtualFileSystem, VertexBuffer, VertexBufferBuilder
│   ├── casc/storage.h          #   RAII CascLib wrapper (Storage class)
│   ├── m2/                     #   WoW M2 format (parser.h, writer.h, structures.h, types.h)
│   ├── m3/                     #   SC2 M3 format (same pattern)
│   ├── mdx/                    #   WC3 MDX format (same pattern)
│   ├── sno/                    #   Diablo SNO (sno_reader.h, sno_value.h, sno_types.h, core_toc.h)
│   └── textures/               #   Texture class + format-specific public headers
│       ├── texture.h           #     Texture, PixelFormat, TextureKind, TextureType
│       ├── blp/                #     BLP1/BLP2 (blp.h, parser.h, writer.h, types.h)
│       ├── bmp/                #     BMP (bmp.h, parser.h, writer.h)
│       ├── dds/                #     DDS (dds.h, parser.h, writer.h)
│       ├── tex/                #     Blizzard TEX (tex.h, parser.h, writer.h, types.h)
│       └── tga/                #     TGA (tga.h, parser.h, writer.h)
├── src/whiteout/               # PRIVATE IMPLEMENTATION
│   ├── common/                 #   binary_reader.h, binary_writer.h, concepts.h, streams.h
│   ├── m2/                     #   parser.cpp, writer.cpp, file_system, binary_*_visitor/
│   ├── m3/                     #   parser.cpp, writer.cpp, binary_*_visitor (*.inl)
│   ├── mdx/                    #   parser.cpp, writer.cpp
│   ├── sno/                    #   sno_reader.cpp, sno_defs.h/.cpp, sno_value.cpp, core_toc.cpp
│   │   ├── d3/                 #   D3-specific auto-generated type registry
│   │   └── d4/                 #   D4-specific auto-generated type registry
│   ├── textures/               #   texture.cpp, format parsers/writers, codec suite
│   │   ├── bcn/                #     BC1–BC7 encode/decode (bc1.cpp … bc7.cpp, bcn_common.cpp)
│   │   ├── bcn.h / bcn.cpp     #     Format-agnostic BCn dispatcher
│   │   ├── blp/                #     BLP1/BLP2 parser + writer
│   │   ├── bmp/                #     BMP parser + writer
│   │   ├── dds/                #     DDS parser + writer
│   │   ├── jpeg/               #     Custom Huffman + JPEG (no libjpeg dependency)
│   │   ├── mipmap/             #     Mipmap generation pipeline (filters, stages, generator)
│   │   ├── tex/                #     Blizzard TEX parser + writer
│   │   ├── tga/                #     TGA parser + writer (uncompressed + RLE)
│   │   ├── utils/              #     Shared texture utilities
│   │   │   ├── pixel_convert.h #       Per-pixel format converters via RGBA32F intermediate
│   │   │   ├── srgb_linearize.h#       sRGB ↔ linear conversion with LUT
│   │   │   ├── quantize.h/.cpp #       Wu's color quantization for palette generation
│   │   │   ├── blue_noise.h/.cpp#      Void-and-Cluster blue-noise threshold map
│   │   ├── io_helpers.h        #     File I/O utilities (read_file_bytes, write_file_bytes)
│   │   └── issue_sink.h        #     Base class for strict/lenient error reporting
│   └── casc/storage.cpp        #   CascLib C-API bridge
├── examples/                   # Standalone example programs (one per format)
├── docs/                       # File-format specs (BLP, M2, M3, MDX, SNO, CASC)
└── scripts/                    # Code generators (e.g. gen_sno_defs.py, gen_d3_sno_defs.py)
```

---

## 3. Build Targets

All defined in the single `CMakeLists.txt`:

| Target | Type | Description |
|--------|------|-------------|
| `whiteout_lib` | Static library | Core library — all formats except CASC |
| `whiteout_casc` | Static library | CASC wrapper, links `whiteout_lib` + `casc_static` |
| `*_example` | Executables | Example programs in `examples/` (gated by `WHITEOUT_BUILD_EXAMPLES`) |

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `WHITEOUT_ENABLE_CASC` | OFF | Fetches CascLib via FetchContent, builds `whiteout_casc` |
| `WHITEOUT_BUILD_EXAMPLES` | OFF | Builds example programs |

**Build commands:**
```
cmake -S . -B build
cmake --build build --config Release
```

---

## 4. Module Architecture

### 4.1 Common Layer (`include/whiteout/` root + `src/whiteout/common/`)

Foundation types shared by all modules:

| File | Key Types | Notes |
|------|-----------|-------|
| `common_types.h` | `u8 u16 u32 u64 i8 i16 i32 i64 f16 f32 f64`, `snorm<T>`, `unorm<T>`, `fixed_point<T,N>` | Numeric primitives. `f16` has `from_float()` / `to_float()`. |
| `vector_types.h` | `Vector2f Vector3f Vector4f Quaternion Matrix44f` | CRTP `VectorMethods<T>` mixin. Supports lerp/slerp/hermite/bezier. |
| `compatibility.h` | `std::optional`, `std::span` shims | Enables C++11 consumers to use the public API. |
| `utils.h` | `VirtualFileSystem`, `VertexBuffer`, `VertexBufferBuilder` | VFS is abstract; VertexBuffer = interleaved GPU-ready data. |
| `common/binary_reader.h` | `BinaryReader` | **Internal.** Reads POD types from `std::istream`. `read<T>()`, `readString()`, `setPosition()`. |
| `common/binary_writer.h` | `BinaryWriter` | **Internal.** Writes POD types to `std::ostream`. Alignment padding. |
| `common/concepts.h` | `BinaryBlob`, `TrivialContiguousRange` | **Internal.** C++20 concepts gating binary I/O. |
| `common/streams.h` | `span_streambuf`, `vector_streambuf` | **Internal.** Adapts `std::span<const u8>` / `std::vector<u8>` to iostream. |

### 4.2 MDX Module — Warcraft III Models

**Public:** `include/whiteout/mdx/{mdx.h, parser.h, writer.h, structures.h, types.h}`
**Internal:** `src/whiteout/mdx/{parser.cpp, writer.cpp}`

- **Chunk-based** binary format. Each chunk: 4-byte FourCC tag + 4-byte size.
- `types.h` defines 50+ chunk tag constants (`MDLX_TAG`, `VERS_TAG`, `GEOS_TAG`, …).
- `structures.h` defines `Model` root + `Sequence`, `Texture`, `Material`, `Geoset`, `Bone`, `Node`, particle emitters, etc.
- Parser supports version upgrade mode (`UpgradeOldVersions` / `PreserveOriginal`).
- PImpl pattern hides implementation.

**Data flow:** buffer → `BinaryReader` → chunk loop → populate `Model` struct → return.

### 4.3 M2 Module — World of Warcraft Models

**Public:** `include/whiteout/m2/{m2.h, parser.h, writer.h, structures.h, types.h}`
+ `structures/{base.h, skin.h, anim.h, bone.h, chunks.h, phys.h, skeleton.h}`
**Internal:** `src/whiteout/m2/{parser.cpp, writer.cpp, file_system.h/.cpp}`
+ `binary_parse_visitor/{base,chunk,skin,bone,anim}.cpp`
+ `binary_writer_visitor/{base,chunk,skin,bone,anim}.cpp`

- Two sub-formats: **MD20** (monolithic) and **MD21** (chunked, Legion+).
- `FileSystem` bundles: one `.m2` base + multiple `.skin` LODs + `.anim` files + optional `.skel`/`.bone`.
- `file_system.cpp` auto-discovers sibling files on disk.
- **Visitor pattern:** `BinaryParseVisitor` dispatches to per-structure parsers.
- `types.h` has `lazy_vector<T>` for deferred M2Array resolution, `CompatQuaternion` (compressed snorm16×3 + unorm16).
- PImpl pattern hides implementation.
- Version constants span WoW Vanilla (256) through Shadowlands (274).

**Data flow:** path → `file_system` discovers bundle → parse each file type → `BinaryParseVisitor` → populate `FileSystem` → return.

### 4.4 M3 Module — StarCraft II / Heroes of the Storm Models

**Public:** `include/whiteout/m3/{m3.h, parser.h, writer.h, structures.h, types.h}`
+ `structures/{anim,base,effect,material,mesh,misc,physics,scene,types}.h`
**Internal:** `src/whiteout/m3/{parser.cpp, writer.cpp, types.cpp}`
+ `binary_parse_visitor.cpp` (includes `.inl` per-domain files)
+ `binary_writer_visitor.cpp` (includes `.inl` per-domain files)

- **Block-based** format: MD33/MD34 header → indexed reference blocks.
- 100+ FourCC tags in `types.h` (`TAG_MD34`, `TAG_MODL`, `TAG_SEQS`, …).
- **Version-aware** field reading — structs vary across M3 versions (v23–v30).
- `Model` is the root (784–868 bytes depending on version).
- `.inl` files are inline implementations included into the `.cpp` visitors.
- PImpl pattern hides implementation.

**Data flow:** buffer → MD33/MD34 header → index table → visit each referenced block → populate `Model` → return.

### 4.5 Texture Module — BLP, DDS, BMP, TGA, TEX

**Public:** `include/whiteout/textures/texture.h`
+ `blp/{blp.h, parser.h, writer.h, types.h}`, `dds/{dds.h, parser.h, writer.h}`,
  `bmp/{bmp.h, parser.h, writer.h}`, `tga/{tga.h, parser.h, writer.h}`,
  `tex/{tex.h, parser.h, writer.h, types.h}`
**Internal:** `src/whiteout/textures/` tree

- **`Texture`** is the format-agnostic interchange type (PImpl). Holds pixel data + mip chain + metadata.
- `PixelFormat` enum: `R8`, `R16`, `R32F`, `RG8`, `RG16`, `RG32F`, `RGBA8`, `RGBA16`, `RGBA32F`, `BC1`–`BC7`.
- `TextureKind` enum (11 values): `Other`, `Diffuse`, `Normal`, `Specular`, `ORM`, `Albedo`, `Roughness`, `Metalness`, `AmbientOcclusion`, `Gloss`, `Emissive`.
- `TextureType` enum: `Texture2D`, `Texture3D`, `TextureCube`.
- Factory methods: `create2D()`, `create3D()`, `createCube()`.
- Format conversion: `format(PixelFormat)` (in-place), `copyAsFormat(PixelFormat)` (non-destructive).
- `generateMipmaps()` — generates full mip chain with kind-aware pipeline selection.
- sRGB flag: `isSrgb()` / `setSrgb(bool)`.
- Five format parser/writer pairs: `blp/`, `bmp/`, `dds/`, `tga/`, `tex/`.
- Full **BCn codec suite** (`bcn/bc1.cpp` through `bcn/bc7.cpp`) — no external dependency.
- Custom **JPEG encoder/decoder** (`jpeg/`) — no libjpeg dependency.
- `utils/quantize.cpp` implements Wu's color quantization for palette generation.
- `utils/blue_noise.cpp` provides the Void-and-Cluster blue-noise threshold map used by dithering.

#### Mipmap Generation Pipeline (`src/whiteout/textures/mipmap/`)

- **`generator.h`** — Public API: `generateMipmaps(Texture&)` with automatic pipeline selection per `TextureKind`.
- **`pipeline.h`** — Configurable pipeline: pre-process stages → downsample filter → post-process stages.
- **`filters.h`** — Downsample filters: `boxFilter()`, `lanczos3Filter()`, `kaiserFilter(β)`.
- **`stages.h`** — Processing stages: gamma linearize/delinearize, normal-map unpack/pack/renormalize/Toksvig correction, roughness square/unsquare, gloss↔roughness conversion.
- **`mip_image.h`** — Intermediate float-per-channel image buffer.
- Every mip level is generated directly from the base (mip 0) — eliminates cascading blur.
- Pipeline auto-selected per `TextureKind`:
  - **Diffuse/Albedo:** Lanczos3 + sRGB linearize/delinearize
  - **Normal:** Kaiser(β=6) + unpack/Toksvig/renormalize/pack
  - **Roughness:** Kaiser(β=6.5) variance-preserving (r→r², filter, √r)
  - **Other kinds:** Kaiser variants with kind-specific β values

#### Internal Helpers (`src/whiteout/textures/`)

| File | Purpose |
|------|---------|
| `bcn.h` / `bcn.cpp` | Format-agnostic BCn dispatcher |
| `io_helpers.h` | File I/O utilities (`read_file_bytes`, `write_file_bytes`) |
| `issue_sink.h` | Base class for strict/lenient error reporting |
| `utils/pixel_convert.h` | Per-pixel format converters via RGBA32F intermediate |
| `utils/srgb_linearize.h` | sRGB ↔ linear conversion with LUT |
| `utils/quantize.h/.cpp` | Wu's color quantization for palette generation |
| `utils/blue_noise.h/.cpp` | Void-and-Cluster blue-noise threshold map |

**Data flow:** raw file → format-specific parser → `Texture` object (decoded to RGBA or keeps compressed) → optional `convertFormat()` / `generateMipmaps()` → format-specific writer.

### 4.6 SNO Module — Diablo III & IV Data Files

**Public:** `include/whiteout/sno/{sno_reader.h, sno_value.h, sno_types.h, core_toc.h}`
**Internal:** `src/whiteout/sno/{sno_reader.cpp, sno_defs.h/.cpp, sno_value.cpp, sno_types.cpp, core_toc.cpp}`
+ `d3/sno_defs.{h,cpp}` and `d4/sno_defs.{h,cpp}` (auto-generated)

This is the most complex module. Key concepts:

#### Type System
- **`SnoTypeDef`** — describes a type (hash, size, isBasic, field count).
- **`SnoFieldDef`** — describes a field (3-element type hash chain, offset, flags, arrayLength, group).
- **`SnoTypeRegistry`** — abstract interface. Implementations in `d3/` and `d4/`.
- D4 resolves root types via **format hash** (from file header byte 4–7).
- D3 resolves root types via **SNO group ID** (passed by caller).

#### Value Tree
- **`SnoValue`** — discriminated union (null, bool, int, uint, float, i64, u64, byte, word, string, vec2/3/4, ivec2, color, colorf, ref, gbid, array, object).
- **`SnoArray`** — homogeneous typed arrays (`std::vector<u8>`, `<f32>`, `<SnoVec3>`, …) or heterogeneous (`std::vector<SnoValue>`).
- **`SnoObject`** = `std::map<std::string, SnoValue>` — field name → value.

#### Reader Architecture (`sno_reader.cpp`)
- `ReadCtx` — carries `BinaryReader`, optional external payload reader, registry ref, size limits, bytes-consumed counter.
- `readStructure()` — **recursive dispatcher**. Looks up `SnoTypeDef`, branches on `isBasic`:
  - Basic → `readBasicType()` (big switch on 30+ `TypeHash::DT_*` constants).
  - Complex → iterate fields, recurse for each.
- Special types: `DT_VARIABLEARRAY` (offset+size indirect), `DT_POLYMORPHIC_VARIABLEARRAY` (runtime type dispatch via `dwType`), `DT_TAGMAP` (self-describing key-value), `DT_FIXEDARRAY`, `DT_CSTRING`, `DT_STRING_FORMULA`, `DT_BINDABLEPROPERTY`, `DT_OPTIONAL`, `DT_RANGE`.
- **Alignment** computed by `getTypeAlignment()` / `getBasicTypeAlignment()` (mirrors parse.js rules).
- **Typed array fast path:** `typedArrayElemSize()` + `readTypedArrayFromBuf()` bulk-read homogeneous arrays.
- **External payload data:** `flags & 0x200000` or `0x400000` → data lives in a separate buffer.
- D3 fallback: `parseD3()` → `readD3Structure()` (different variable-array layout: offset/size at bytes 0–7 instead of 8–15).

#### CoreTOC (`core_toc.cpp`)
- Parses `CoreTOC.dat` — master asset index mapping `(SnoGroup, SnoId)` → name.
- Auto-detects 3 formats: D3 Legacy (no magic), D4 Old (count header), D4 New (magic `0xBCDE6611`).
- Returns `std::vector<TocEntry>` and per-group format hashes.

#### SNO Groups
- `SnoGroup` enum: 150+ asset categories (Actor, Animation, Material, Texture, Sound, etc.).
- Each group has a numeric ID used as the CASC file-ID prefix.

### 4.7 CASC Module — Archive Access

**Public:** `include/whiteout/casc/storage.h`
**Internal:** `src/whiteout/casc/storage.cpp`

- RAII `Storage` class wrapping CascLib's C API.
- `open(path)` / `openOnline(codeName)` → `readFile(cascPath | fileId)` → `std::optional<std::vector<u8>>`.
- `enumerate(mask, listFile, callback)` for wildcard file discovery.
- **Optional build target** (`whiteout_casc`), gated by `WHITEOUT_ENABLE_CASC`.

---

## 5. Design Patterns & Conventions

### 5.1 Patterns Used

| Pattern | Where | Purpose |
|---------|-------|---------|
| **PImpl** | All parsers/writers, `Texture` | Hide C++20 internals from C++11 public headers |
| **Visitor** | M2 `BinaryParseVisitor`, M3 visitor | Dispatch chunk/block parsing to per-domain handlers |
| **Registry / Singleton** | `d3::SnoTypeRegistry::instance()`, `d4::SnoTypeRegistry::instance()` | Game-specific type lookup tables |
| **Discriminated Union** | `SnoValue` | Type-safe heterogeneous value container |
| **CRTP Mixin** | `VectorMethods<T>` | Shared vector math operations |

### 5.2 Naming Conventions

- **Namespaces:** `whiteout::mdx`, `whiteout::m2`, `whiteout::m3`, `whiteout::sno`, `whiteout::textures`, `whiteout::casc`.
- **Files:** `snake_case.cpp/.h`. Public headers in `include/whiteout/`, private in `src/whiteout/`.
- **Types:** `PascalCase` for classes/structs/enums, `camelCase` for methods, `m_` prefix for member variables.
- **Constants:** `kCamelCase` for local constants, `ALL_CAPS` for tag constants.
- **Auto-generated files** are clearly marked with `AUTO-GENERATED by scripts/...` in the header comment.

### 5.3 C++11 / C++20 Boundary

The public API in `include/` uses only C++11 features (with `compatibility.h` shims for `std::optional` and `std::span`). Internal code in `src/` freely uses C++20 concepts, designated initializers, `std::span`, etc. The PImpl idiom enforces this boundary.

### 5.4 Error Handling

- Parsers return `std::optional` (nullopt on failure) or populate an issues list (`getIssues()`).
- `ParseMode::Lenient` collects non-fatal issues and continues; `Strict` throws on first error.
- No exceptions cross the public API boundary in normal operation.

---

## 6. Key Data Flows

### Reading a Diablo IV SNO file from CASC
```
casc::Storage::readFile(fileId)
  → std::vector<u8> raw data
    → SnoReader::parse(data, group, payloadData)
      → validate magic (0xDEADBEEF)
      → read formatHash from header bytes 4-7
      → d4::SnoTypeRegistry::typeHashFromKey(formatHash)  → rootTypeHash
      → d4::SnoTypeRegistry::findType(rootTypeHash)       → SnoTypeDef*
      → BinaryReader over payload (offset 16)
      → readStructure(ctx, rootTypeHash, offset=0)
        → for each field: readStructure(ctx, field.typeHashes, baseOffset + field.offset)
          → readBasicType() for leaves (DT_INT, DT_FLOAT, DT_VARIABLEARRAY, ...)
      → SnoFile { signature, formatHash, snoId, typeName, root: SnoValue }
```

### Reading a WoW M2 model from disk
```
m2::Parser::parse(filePath)
  → file_system: discover .m2 + .skin + .anim + .skel + .bone siblings
  → parse base .m2:
    → detect MD20 vs MD21
    → BinaryParseVisitor dispatches to base/chunk/skin/bone/anim handlers
    → populate BaseFile
  → parse each .skin → SkinFile
  → parse each .anim → AnimFile
  → optionally parse .skel → SkeletonFile
  → FileSystem { base, skins[], anims[], skeleton? }
```

### Converting a texture between formats
```
textures::Texture tex = blp::parse(blpData)   // → Texture (maybe BC1 compressed)
tex.convertFormat(PixelFormat::RGBA8)          // → decode BC1 to RGBA8
tex.generateMipmaps()                          // → kind-aware mip chain generation
std::vector<u8> ddsBytes = dds::write(tex)     // → re-encode as DDS
```

---

## 7. File Modification Cheat Sheet

> When modifying the project, use this to find the right files quickly.

| If you need to… | Look in… |
|-----------------|----------|
| Add a new SNO basic type | `src/whiteout/sno/sno_reader.cpp` → `TypeHash` namespace + `readBasicType()` switch |
| Add a new D4 SNO type/field | Run `scripts/gen_sno_defs.py` (regenerates `d4/sno_defs.cpp`) |
| Add a new D3 SNO type/field | `src/whiteout/sno/d3/sno_defs.cpp` (auto-generated via `scripts/gen_d3_sno_defs.py`) |
| Fix SNO alignment issues | `sno_reader.cpp` → `getTypeAlignment()` / `getBasicTypeAlignment()` |
| Fix SNO variable array parsing | `sno_reader.cpp` → `DT_VARIABLEARRAY` / `DT_POLYMORPHIC_VARIABLEARRAY` cases |
| Add M2 chunk support | `src/whiteout/m2/binary_parse_visitor/chunk.cpp` + `include/whiteout/m2/structures/chunks.h` |
| Fix M2 bone/anim parsing | `src/whiteout/m2/binary_parse_visitor/{bone,anim}.cpp` |
| Add M3 material type | `include/whiteout/m3/structures/material.h` + `src/whiteout/m3/binary_parse_visitor.cpp` (material.inl) |
| Add BCn codec or fix encoding | `src/whiteout/textures/bcn/{bc1..bc7}.cpp` |
| Add new texture format | New dir under `src/whiteout/textures/` + `include/whiteout/textures/`, parser/writer pair, update `CMakeLists.txt` |
| Fix BLP palette/JPEG issues | `src/whiteout/textures/blp/parser.cpp` or `writer.cpp` |
| Fix BMP parsing/writing | `src/whiteout/textures/bmp/parser.cpp` or `writer.cpp` |
| Fix TGA parsing/writing | `src/whiteout/textures/tga/parser.cpp` or `writer.cpp` |
| Modify mipmap pipeline | `src/whiteout/textures/mipmap/` — `generator.cpp` (kind→pipeline mapping), `filters.cpp`, `stages.cpp` |
| Add new TextureKind pipeline | `src/whiteout/textures/mipmap/generator.cpp` → pipeline selection switch |
| Add mipmap filter or stage | `src/whiteout/textures/mipmap/filters.h/.cpp` or `stages.h/.cpp` |
| Fix pixel format conversion | `src/whiteout/textures/utils/pixel_convert.h` or `texture.cpp` |
| Add CASC features | `src/whiteout/casc/storage.cpp` + `include/whiteout/casc/storage.h` |
| Add CoreTOC format variant | `src/whiteout/sno/core_toc.cpp` |
| Add new example program | New `.cpp` in `examples/`, add `add_executable` + `target_link_libraries` in `CMakeLists.txt` |
| Change public API | Edit `include/whiteout/` headers (keep C++11 compatible!) |
| Change internal binary I/O | `src/whiteout/common/binary_reader.h` or `binary_writer.h` |

---

## 8. Important Constants & Magic Numbers

| Constant | Value | Where | Meaning |
|----------|-------|-------|---------|
| SNO magic | `0xDEADBEEF` | All SNO files byte 0–3 | File signature |
| CoreTOC D4 new magic | `0xBCDE6611` | CoreTOC.dat byte 0–3 | D4 new-format TOC |
| Polymorphic base hash | `0x5d4bac71` | sno_reader.cpp | `DT_POLYMORPHIC_VARIABLEARRAY` base type |
| MD20 tag | `0x3032444D` | M2 files | WoW model magic |
| MD21 | chunked MD20 | M2 files (Legion+) | Chunked wrapper |
| MD33/MD34 | `TAG_MD33`/`TAG_MD34` | M3 files | SC2 model headers |
| MDLX | `0x584C444D` | MDX files | WC3 model magic |
| External payload flags | `0x200000`, `0x400000` | SNO field flags | Data lives in separate `.payload` buffer |
| TEX magic | `0xDEADBEEF` | TEX files | Blizzard texture signature |
| TEX version | 47 | TEX files | Current format version |

---

## 9. Testing & Validation

- No formal test framework; validation is done via example programs that exercise parse → inspect → write round-trips.
- `m3_round_trip_test`, `bc7_roundtrip_test`, `d3_app_corpus_test` etc. are test-like executables in `examples/`.
- Build all examples and run them against sample files to validate changes.

---

## 10. Gotchas & Non-Obvious Behaviors

1. **D3 vs D4 variable array layout is different.** D4: `{pad(4), pad(4), offset(4), size(4)}`. D3: `{offset(4), size(4), pad(4), pad(4)}`. Both use 16 bytes total.
2. **SNO type hash chains are 3 elements.** `typeHashes[0]` is the container type, `[1]` is the element type, `[2]` is the sub-element type. Unused slots are `DT_NULL`.
3. **`DT_OPTIONAL` reads the sub-value first, then checks the present flag** (at the same offset). The present flag is at offset+0 and the value occupies the full struct size.
4. **Alignment in tagmaps** uses different rules (`inTagMap=true` makes pointer types use 4-byte instead of 8-byte alignment).
5. **Auto-generated `sno_defs.cpp` files are huge** (thousands of lines). Don't edit them by hand — modify the generator script.
6. **The JPEG codec is fully custom** (no libjpeg). It lives in `textures/jpeg/` and handles byte-stuffing, restart markers, DHT/DQT tables.
7. **M3 `.inl` files are not standalone** — they're `#include`d into `binary_parse_visitor.cpp` / `binary_writer_visitor.cpp`.
8. **`whiteout_casc` is a separate library target** that links both `whiteout_lib` and CascLib. Examples needing CASC link against `whiteout_casc`, not `whiteout_lib`.
