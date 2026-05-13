# WhiteoutLib

[![Build status](https://ci.appveyor.com/api/projects/status/github/FernandoS27/WhiteoutLib?branch=master&svg=true)](https://ci.appveyor.com/project/FernandoS27/whiteoutlib)

> **Beta:** WhiteoutLib is in beta. The public API is stabilising and most modules are exercised by real consumers, but breaking changes are still possible between minor versions.

WhiteoutLib is a C++ library for reading and writing the 3D model, texture, and
storage formats used by Blizzard Entertainment games. It is inspired by
StormLib but is a fresh, modern implementation: pure C++20 internals, no
third-party compression or codec dependencies, and a public API designed for
language bindings.

## Why WhiteoutLib

- **Pure C++ CASC implementation.** CASC reading *and writing* are implemented
  in-house. To our knowledge,
  WhiteoutLib is the only open-source library that ships a from-scratch C++
  CASC writer with full BLTE encoding, encoding-table, and root-handler
  support.
- **Zero external codec dependencies.** Every codec the library needs — BCn
  (BC1/2/3/4/5/6H/7), JPEG baseline (for BLP1), PNG, Wu colour quantisation,
  mipmap filters — is implemented in-house. There is no zlib, bzip2, libjpeg,
  libpng, or stb_image link dependency, so the library composes cleanly with
  any host project's own copies of those libraries.
- **Buffer-first parser API.** Every parser accepts an in-memory
  `std::span<const u8>`. That makes it trivial to compose with any storage
  backend (CASC, MPQ, loose files, network, in-memory archives) without
  per-format plumbing.
- **`std::optional`-based error model.** Parsers return `std::optional<T>` by
  default and offer an opt-in strict mode that throws. No surprise exceptions
  from the happy path, and lenient parsers surface issues via an inspectable
  issue list instead of aborting.
- **Multithreaded by default where it counts.** CASC indexing, file
  enumeration, and bulk operations use a worker pool and timeline semaphores;
  measured throughput beats CascLib on most CASC paths we have benchmarked.
- **PImpl public surface.** Every heavy type (`Storage`, `Texture`,
  `models::*::Parser`/`Writer`) hides its implementation behind a
  `std::unique_ptr<Impl>`. This keeps the ABI surface small and stable, and
  makes language bindings practical.
- **C++20 internals, C++11-compatible public headers.** The implementation
  uses C++20 freely; the public headers fall back to compatibility shims
  (`std::optional`/`std::span` polyfills) so binding generators that target
  older standards still work.
- **Round-trip read/write for most formats.** MDX, M3, BLP, DDS, TEX, PNG,
  JPEG, BMP, TGA all support both directions. The texture pipeline is
  exercised in production by [WhiteoutTex](https://github.com/FernandoS27/WhiteoutTex),
  which uses it for viewing, conversion, mipmap generation, and Blizzard
  texture workflows.

## Module Maturity

- **Textures** and **Storage** are the most mature modules and have seen
  thorough testing. The texture module is exercised extensively by
  [WhiteoutTex](https://github.com/FernandoS27/WhiteoutTex).
- **Models** is mixed:
  - **MDX** has had the heaviest testing.
  - **M3** can round-trip read and write StarCraft II and Heroes of the Storm
    models.
  - **M2** is still experimental — missing version-specific parsing for older
    than Mists of Pandaria, and no `.phys` / `.bone` support yet.
- **WEM** is an experimental intermediate model format intended to become the
  bridge between the other model formats.
- **SNO** is currently the weakest module:
  - SNO is a binary JSON-style data type system used by Diablo III/IV.
  - Diablo IV support still requires more reverse engineering for some payload
    data types.
  - Diablo III support is currently quite weak.

## Format Support

### 3D models

- **Warcraft III (`.mdx`)** — Classic and Reforged
- **World of Warcraft (`.m2`)** — experimental; partial, see above
- **StarCraft II / Heroes of the Storm (`.m3` / `.m3a`)**
- **Diablo III & IV (`.acr`, `.app`, `.ani`, `.ans`, `.mat`, `.prt`, SNO format family)**
- **WEM** — experimental intermediate format

### Textures

- **Warcraft III & World of Warcraft (`.blp`)** — both BLP1 (Warcraft III
  Classic) and BLP2 (World of Warcraft)
- **Direct3D Surface (`.dds`)**
- **Diablo III & IV (`.tex`)**
- **Standard formats** — `.jpeg`, `.bmp`, `.png`, `.tga`, plus `.gif` (write only)
- **Mipmap generation** for PBR and legacy pipelines, with texture-type and
  channel-semantics awareness

### Virtual file systems

- **CASC** — pure C++ in-house implementation, read + write, with improved
  handling for Diablo III/IV root formats
- **MPQ** — optional Warcraft III archive reader/writer

## Language Bindings

WhiteoutLib's public headers are designed to be binding-friendly: PImpl
ownership, no exposed templates beyond the math types, no STL containers in
hot return positions, and a uniform error-handling convention.

All bindings are produced by an in-house Python codegen tool
([tools/codegen/](tools/codegen/)) that parses annotated C++ headers via
libclang into a backend-neutral IR, then emits backend-specific binding
source. The same `@bind` annotations on the C++ declarations drive every
target language, so adding a new binding surface is a matter of running the
codegen rather than hand-writing wrappers.

Bindings work is currently in progress:

| Language | Status | Location | Codegen backend |
|---|---|---|---|
| **JavaScript / TypeScript (WebAssembly)** | In progress, alpha | [packages/js-ts/](packages/js-ts/) | Embind (+ `.d.ts` stubs) |
| **Python** | In progress, alpha | [bindings/python/](bindings/python/) | pybind11 (+ `.pyi` stubs) |
| **Java** | Early | — | JNI |
| **Flat C ABI** | Early | — | hand-curated C |
| **C#** | Planned | — | TBD |

The WASM and Python bindings currently cover the model and texture modules;
CASC, MPQ, and the networking surface are not yet exposed through bindings.

## Format References

See [docs/](docs/) for per-format specifications.

## Build

CMake-based:

```bash
cmake -S . -B build
cmake --build build --config Release
```

Useful options:

| Option | Default | What it does |
|---|---|---|
| `WHITEOUT_ENABLE_CASC` | `OFF` | Build the `whiteout_casc` static library |
| `WHITEOUT_ENABLE_MPQ` | `OFF` | Build the `whiteout_mpq` static library |
| `WHITEOUT_BUILD_EXAMPLES` | `OFF` | Build the example programs under `examples/` |
| `WHITEOUT_BUILD_TESTS` | `OFF` | Build the test suite |
| `WHITEOUT_BUILD_WASM_BINDINGS` | `ON` if Emscripten | Build the WebAssembly bindings |
| `WHITEOUT_BUILD_PYTHON_BINDINGS` | `OFF` | Build the Python bindings |
| `WHITEOUT_ENABLE_CLANG_TIDY` | `OFF` | Run clang-tidy during the build |
| `WHITEOUT_WARNINGS_AS_ERRORS` | `ON` for master project | Treat warnings as errors |

## Static analysis (clang-tidy)

`.clang-tidy` at the repo root configures checks. CMake always emits
`compile_commands.json` next to the build outputs, which both `clang-tidy`
and `clangd` consume directly.

```bash
# Single file
clang-tidy -p build src/whiteout/storages/casc/codec/crypto.cpp

# Whole tree (parallel)
run-clang-tidy -p build -header-filter='(include|src)[/\\]whiteout[/\\]'

# Convenience wrapper (PowerShell)
pwsh tools/run-clang-tidy.ps1 -BuildDir build
```

`-DWHITEOUT_ENABLE_CLANG_TIDY=ON` hooks clang-tidy into the build itself
(via `CMAKE_CXX_CLANG_TIDY`). This works for clang/gcc; the option is a
no-op for clang-cl on Windows (CMake's wrapper feeds clang-tidy a mangled
invocation there — use the standalone flow above).

## Examples

Example programs live in [examples/](examples/), covering loading and writing
each supported format.

## License

BSD 3-Clause. See [LICENSE](LICENSE).

## Third-Party Notices

This project includes and/or references third-party components.
See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for details.

## Disclaimer

This project is not affiliated with or endorsed by Blizzard Entertainment.
