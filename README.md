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

### Client databases

- **DBC / DB2** — optional reader for every container revision from `WDBC`
  through `WDC5`, including bitpacked, pallet and common-data fields,
  multi-section files, sparse (offset-map) records, copy tables and
  relationship maps. Encrypted sections are reported rather than guessed at.
  Columns can be named and typed by binding a `Schema`.

## Language Bindings

WhiteoutLib ships five first-class language bindings:

| Language | Runtime | Package | Location |
|---|---|---|---|
| **JavaScript / TypeScript** | WebAssembly (browser + Node.js) | — | [packages/js-ts/](packages/js-ts/) |
| **Python** | CPython 3.10+ extension module | — | [bindings/python/](bindings/python/) |
| **Java** | JDK 22+ via Foreign Function & Memory API | — | [bindings/java/](bindings/java/) |
| **C#** | .NET 8+ via `[LibraryImport]` P/Invoke (AOT-clean) | — | [bindings/csharp/](bindings/csharp/) |
| **Rust** | 1.70+, links the C ABI directly | [`whiteoutlib`](https://crates.io/crates/whiteoutlib) · [docs](https://docs.rs/whiteoutlib) | [bindings/rust/](bindings/rust/) |

The Rust crate is published as **`whiteoutlib`** (`whiteout` was taken), but
the import path stays `whiteout`:

```toml
[dependencies]
whiteoutlib = { version = "0.1", features = ["casc", "mpq"] }
```

```rust
use whiteout::textures::{BlpParser, PixelFormat};
```

It builds the bundled C++ with CMake by default (the `vendored` feature);
set `WHITEOUT_LIB_DIR` to link a prebuilt `whiteout_native` instead.

A flat C ABI also exists under [bindings/c/](bindings/c/), but it is **not
intended for direct use** — it exists purely as a stable bridge that the
Java FFM and C# P/Invoke layers (and any future non-C++ language) call
into. The same `whiteout_native.dll` / `libwhiteout_native.so` backs every
binding; consumers can share one staged native binary across runtimes.
End users should pick one of the five high-level bindings above.

### Generated, not hand-written

All bindings are produced by an in-house Python codegen tool
([tools/codegen/](tools/codegen/)) that parses annotated C++ headers via
libclang into a backend-neutral IR, then emits backend-specific source for
each target:

- **Embind** (`.cpp` + `.d.ts`) for the WASM bindings
- **pybind11** (`.cpp` + `.pyi`) for the Python bindings
- **Java + C ABI** (`.java` + `extern "C"` shim) for the FFM bindings
- **C#** (`.cs` with `[LibraryImport]` source-generated P/Invoke) sharing the same C ABI
- **Rust** (`.rs` with `extern "C"` declarations) over the same C ABI

The same `@bind` annotations on C++ declarations drive every backend, so
adding or modifying a binding is a single-source change in the C++ header
followed by a codegen run — never a hand-edit of generated files.

### Designed to feel idiomatic in each language

The bindings are intentionally **not** a thin 1:1 mirror of the C++ API.
Each backend reshapes the surface to match the target language's
conventions: snake_case fields in Python, camelCase in JS/TS, JavaBeans
getters and `AutoCloseable` lifetime in Java, `Optional<T>` for fallible
parsers, native flag enums where each language has them, and per-language
math-type ergonomics. The goal is that idiomatic code in each language
reads as if the library had been written for that language directly.

### Zero-copy buffer access

Python, JavaScript, and C# bindings all expose the underlying C++
`std::vector` storage as a native buffer view in the host language — **no
copy, mutations write through**.

#### Python — NumPy buffer protocol

Primitive vectors and the standard math types (`Vector2f/3f/4f`,
`Quaternion`, `ColorBGRA`) implement the buffer protocol, so
`np.asarray(...)` returns a view aliased to the C++ memory:

```python
import numpy as np
import whiteout as w

model = w.mdx.Parser().parse_buffer_format(
    open("model.mdx", "rb").read(), w.mdx.MDLXFormat.MDX)

# Primitive vector → 1D array
keys = np.asarray(track.keys)               # dtype=float32, shape=(N,)
keys[:] *= 2.0                              # writes through to C++

# Math-struct vector → 2D array
pivots   = np.asarray(model.pivot_points)   # shape=(N, 3), dtype=float32
tangents = np.asarray(geo.tangents)         # shape=(N, 4), dtype=float32
pivots[:, 1] += 1.0                         # bulk Y-offset, no copy
```

#### TypeScript / JavaScript — TypedArray views

The WASM bindings expose a matching `.view()` method on every primitive
and math-struct vector that returns a JS `TypedArray` aliased to the WASM
heap:

```ts
import { Whiteout } from "whiteout-wasm";
const w = await Whiteout();

const model = w.mdx.parse(mdxBytes);

// Primitive vector → 1D TypedArray
const keys: Float32Array = model.globalSequences.get(0).keys.view();
keys[1] = 99.0;                             // writes through

// Math-struct vector → flat TypedArray, laid out [x0,y0,z0, x1,y1,z1, …]
const pivots: Float32Array = model.pivotPoints.view();
const y1 = pivots[1 * 3 + 1];
```

Element types map to the natural TypedArray for the target
(`u8`→`Uint8Array`, `f32`/`Vector3f`/`Quaternion`→`Float32Array`, …).

#### C# — `ReadOnlySpan<T>` over native memory

The C# bindings alias the standard math types directly to
`System.Numerics` (`Vector3f` → `System.Numerics.Vector3`, etc.) and
expose `std::vector<MathType>` fields as zero-copy `ReadOnlySpan<T>`:

```csharp
using Whiteout.Mdx;

using var parser = new Parser();
using var model  = parser.ParseBufferFormat(mdxBytes, MDLXFormat.MDX);

ReadOnlySpan<System.Numerics.Vector3> pivots = model.PivotPoints;
var first = pivots[0];                          // no allocation
```

This is the same backing buffer the C++ side reads from, surfaced through
.NET's `ReadOnlySpan<T>` (or `Span<T>` for mutable contexts). Combined
with the `System.Numerics` alias, a Whiteout model's pivot points flow
into any SIMD-friendly graphics code without conversion.

**Caveat for all three backends:** any operation that may reallocate the
underlying C++ vector (`append`, `push_back`, `resize`, …) invalidates the
view. Re-acquire (`np.asarray(...)` / `.view()` / re-read the property)
after a size change, or copy out (`np.array(...)` / `new Float32Array(view)` /
`.ToArray()`) if you need a stable snapshot.

### Per-binding documentation

Each binding has its own README with the full API tour, build instructions,
and per-language gotchas:

- [bindings/python/README.md](bindings/python/README.md)
- [packages/js-ts/README.md](packages/js-ts/README.md)
- [bindings/java/README.md](bindings/java/README.md)
- [bindings/csharp/README.md](bindings/csharp/README.md)

The WASM, Python, Java, and C# bindings all cover the model, texture, MPQ,
CASC, and host-extras surfaces — they are generated from the same IR, so
adding or changing a binding is a single-source change in the C++ headers.

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
| `WHITEOUT_ENABLE_DATABASE` | `OFF` | Build the `whiteout_database` static library (DBC/DB2) |
| `WHITEOUT_BUILD_EXAMPLES` | `OFF` | Build the example programs under `examples/` |
| `WHITEOUT_BUILD_TESTS` | `OFF` | Build the test suite |
| `WHITEOUT_BUILD_WASM_BINDINGS` | `ON` if Emscripten | Build the WebAssembly bindings |
| `WHITEOUT_BUILD_PYTHON_BINDINGS` | `OFF` | Build the Python bindings |
| `WHITEOUT_ENABLE_CLANG_TIDY` | `OFF` | Run clang-tidy during the build |
| `WHITEOUT_WARNINGS_AS_ERRORS` | `ON` for master project | Treat warnings as errors |
| `WHITEOUT_INSTALL` | `ON` | Generate install + export rules for `find_package(WhiteoutLib)` |

## Install / `find_package`

After `cmake --install build --prefix <prefix>`, downstream CMake projects can pull WhiteoutLib in with:

```cmake
find_package(WhiteoutLib 1.0 REQUIRED COMPONENTS casc mpq)
target_link_libraries(myapp PRIVATE
    WhiteoutLib::whiteout_lib
    WhiteoutLib::whiteout_casc
    WhiteoutLib::whiteout_mpq
)
```

`COMPONENTS casc` / `mpq` are optional; ask for them only if the install was built with the matching `-DWHITEOUT_ENABLE_*=ON` flag (the config script will fail loudly otherwise).

## vcpkg

A vcpkg overlay port lives under `ports/whiteoutlib/`. To install into a vcpkg environment:

```bash
vcpkg install --overlay-ports=path/to/WhiteoutLib/ports whiteoutlib[casc,mpq]
```

Then in your consuming project's `CMakeLists.txt`:

```cmake
find_package(WhiteoutLib CONFIG REQUIRED COMPONENTS casc mpq)
target_link_libraries(app PRIVATE
    WhiteoutLib::whiteout_lib WhiteoutLib::whiteout_casc WhiteoutLib::whiteout_mpq)
```

Features map directly to CMake flags: `whiteoutlib[casc]` ⇒ `WHITEOUT_ENABLE_CASC=ON`, `whiteoutlib[mpq]` ⇒ `WHITEOUT_ENABLE_MPQ=ON`, `whiteoutlib[curl]` ⇒ `WHITEOUT_ENABLE_CURL_HTTP=ON` + pulls `curl` from vcpkg (no-op on Windows, which always uses WinHTTP).

When publishing the port to a custom vcpkg registry, bump `version` in `ports/whiteoutlib/vcpkg.json` and replace the `SHA512 0` placeholder in `portfile.cmake` with the real SHA — `vcpkg install` will print the expected value on first run.

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
