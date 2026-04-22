# WhiteoutLib

> **Alpha Stage:** This library is currently in early alpha. A lot of the design is actively being iterated on and may change significantly between versions.

WhiteoutLib is a C++ library for reading and writing 3D model, texture formats & storage formats used in Blizzard Entertainment games.
It is inspired by StormLib, a library used by older Blizzard games to handle proprietary file formats like BLP, MDX, and MPQ. 

## Module Maturity

- **Textures** and **Storage** are currently the most mature modules and have seen thorough testing.
  - The texture module is also exercised extensively by [WhiteoutTex](https://github.com/FernandoS27/WhiteoutTex), which uses it as the core of its texture viewing, conversion, mipmap generation, and Blizzard texture workflow support.
- **SNO** is currently one of the weaker modules in terms of maturity.
  - SNO is a binary JSON-style data type system used by the Diablo III and Diablo IV engines.
  - Diablo IV support still requires more reverse engineering for some payload data types.
  - Diablo III support is currently quite weak.
- **Models** is mixed in maturity:
  - **MDX** has had the heaviest testing.
  - **M3** can round-trip read and write StarCraft II and Heroes of the Storm models.
  - **M2** is still experimental.
- **WEM** is an experimental intermediate model format intended to become the bridge between the other model formats over time.

## Current 3D Format Support

- **Warcraft III (`.mdx`)**
  - Classic
  - Reforged
- **World of Warcraft (`.m2`)** (experimental; partial, still missing version specific parsing for older than Mist of panadaria and .phys and .bone files)
- **StarCraft II / Heroes of the Storm (`.m3/.m3a`)**
- **Diablo III & IV (`.acr,.app,.ani,.ans,.mat,.prt`, SNO format family)**

SNO module notes:
- SNO is a binary JSON-style data type system used by Diablo III and Diablo IV.
- Overall maturity is still weak.
- Diablo IV payload data types still need additional reverse engineering.
- Diablo III support is currently in a weaker state.

Model format maturity notes:
- **MDX** is the most heavily tested model format today.
- **M3** supports full round-trip read/write for StarCraft II and Heroes of the Storm models.
- **M2** remains experimental.

## Experimental Intermediate Model Format

- **WEM** is an experimental model format that will be used as a bridge between the other supported model formats.

## Planned Support

- **Optimized parsers for Diablo III/IV `.app, .ani, .acr, .prt, .mat, .ans`**

## Current Texture Format Support

- **Warcraft III & World of Warcraft (`.blp`)**
    Both BLP1 (Warcraft III Classic) & BLP2 (World of Warcraft)
- **Direct3D Surface (`.dds`)**
- **Diablo 3 & 4 (`.tex`)**
- **Standard formats `.jpeg, .bmp, .png, .tga` and `.gif` for saving only.**
- **Mipmap generation for PBR & Legacy pipelines, minding texture type and characteristics**

Texture maturity notes:
- This is one of the most mature parts of the library and has seen thorough testing.
- It is also used extensively by [WhiteoutTex](https://github.com/FernandoS27/WhiteoutTex), a dedicated texture viewer and converter built around WhiteoutLib's texture capabilities.

## Current Virtual File System Support
- **Optional support of MPQ**
- **Optional support of CASC via WhiteoutLib's own pure C++ implementation**
  - Read and write support for CASC storages
  - Improved handling for Diablo III and Diablo IV root formats
  - Multithreaded code paths and general performance improvements over CascLib in most measured areas
  - CascLib is no longer a direct dependency; it is only used optionally in test targets for cross-validation and performance comparison
- Some formats are easier to parse using CASC, like m2 and sno.

## Language Compatibility

The library internals use C++20, but all public headers are C++11 compatible to make it easier to create bindings for other languages.

## Format References

look in `docs/`

## Build

This project uses CMake.

```bash
cmake -S . -B build
cmake --build build --config Release
```

## Examples

Example programs are available in `examples/` for loading and writing supported formats.

## License

BSD 3-Clause. See [LICENSE](LICENSE).

## Third-Party Notices

This project includes and/or references third-party components.
See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for details.

## Disclaimer

This project is not affiliated with or endorsed by Blizzard Entertainment.
