# whiteoutlib

Rust bindings for [WhiteoutLib](https://github.com/Sahmkow/WhiteoutLib) —
parsers and writers for Blizzard model and texture formats.

| Module | Covers |
|---|---|
| `math` | vectors, quaternions, matrices — `Copy`, no allocation |
| `textures` | BLP, DDS, PNG, JPEG, BMP, TGA, TIFF, GIF |
| `mdx` | Warcraft III models |
| `m2` | World of Warcraft models |
| `m3` | StarCraft II / Heroes of the Storm models |
| `host` | OS file system, thread pool, HTTP handler, game finder |
| `interfaces` | traits the library calls *into* — implement these yourself |
| `casc` | CASC storage — behind the `casc` feature |
| `mpq` | MPQ archives — behind the `mpq` feature |

## Installing

```sh
cargo add whiteoutlib --features casc,mpq
```

The package is `whiteoutlib` (the short name was taken on crates.io), but
the library it provides is `whiteout` — so imports read:

```rust
use whiteout::mdx::Parser;
```

The C++ library is bundled and built for you, so this needs **CMake and a
C++20 compiler** on the machine — and the first build takes a few minutes.
Nothing else is required.

To link a prebuilt library instead, turn the default `vendored` feature off
and point at it:

```sh
cargo add whiteoutlib --no-default-features --features casc,mpq
WHITEOUT_LIB_DIR=/path/to/lib cargo build      # add WHITEOUT_STATIC=1 for the static archive
```

`build.rs` resolves in that order: `WHITEOUT_LIB_DIR` (always wins), then
the vendored build, then `pkg-config`.

The `casc` and `mpq` features mirror the CMake `WHITEOUT_ENABLE_*` options.
Under `vendored` they configure the bundled build; against a prebuilt
library they must match how it was configured, or linking fails —
deliberately, rather than silently missing symbols.

## Linking from another crate

This crate declares `links = "whiteout_native"`: it is the single owner of
the native library, and cargo rejects any other crate in the graph that
also claims to provide it. Build your own copy of WhiteoutLib alongside
this one and you get two sets of identically-mangled C++ symbols — which
links on MSVC, silently keeping one definition of each, and quietly
becomes an ODR violation the moment the two versions drift. Depend on this
crate instead.

Calling in from Rust needs nothing extra; linking this crate's rlib pulls
the native library in. A crate with its own C++ to compile can read what
was built from `build.rs`, via the environment cargo sets for dependents
of a `links` crate:

| Variable | Meaning |
|---|---|
| `DEP_WHITEOUT_NATIVE_LIB_DIR` | directory holding `whiteout_native_static` |
| `DEP_WHITEOUT_NATIVE_INCLUDE` | include root for the C++ headers (`<whiteout/…>`) |
| `DEP_WHITEOUT_NATIVE_HAS_CASC` | `1` if built with CASC, else `0` |
| `DEP_WHITEOUT_NATIVE_HAS_MPQ` | `1` if built with MPQ, else `0` |

The `HAS_*` pair reports the configuration the library was *actually* built
with, so a dependent can match it rather than infer it from its own
features. These keys are a compatibility surface — treat them as public.

They are emitted on every path that produces a usable library. `INCLUDE` is
the one that can be missing: with a prebuilt `WHITEOUT_LIB_DIR` that has no
`include/` beside it, set `WHITEOUT_INCLUDE_DIR` to supply it.

## Working on the bindings

From a checkout, `build.rs` finds the repository above the crate, so the
vendored path works with no setup. For the faster prebuilt loop:

```powershell
./scripts/build-rust.ps1              # codegen + cmake + fmt + clippy + test
./scripts/pack-rust.ps1 -Verify       # stage vendored sources + package
```

## Reading models

```no_run
use whiteout::mdx::{MDLXFormat, Parser};

let bytes = std::fs::read("units/human/footman/footman.mdx")?;
let model = Parser::new().parse(&bytes, MDLXFormat::MDX).expect("parse failed");

for geoset in model.geosets_iter() {
    let positions: &[whiteout::math::Vector3f] = geoset.vertex_positions();
    let faces: &[u16] = geoset.faces();
    println!("{} verts, {} indices", positions.len(), faces.len());
}
# Ok::<(), std::io::Error>(())
```

Vertex data is borrowed straight out of the C++ allocation — nothing is
copied, in either direction:

```rust
# use whiteout::{math::Vector3f, mdx::Model};
# let mut model = Model::new();
model.resize_geosets(1);
let mut geoset = model.geosets_mut(0).unwrap();
geoset.set_vertex_positions(&[Vector3f::new(0.0, 0.0, 0.0)]);
geoset.vertex_positions_mut()[0].z = 1.0;   // writes into C++ memory
```

That is safe because the slice borrows the model: the compiler rejects a
resize, a second view, or a drop of the owner while it is alive.

## Textures

```no_run
use whiteout::textures::{BlpParser, PixelFormat, PngWriter};

let blp = std::fs::read("textures/arthas.blp")?;
let mut texture = BlpParser::new().parse(&blp).expect("not a BLP");
texture.convert_to(PixelFormat::RGBA8);
let png = PngWriter::new().write(&texture);
std::fs::write("arthas.png", &png)?;
# Ok::<(), std::io::Error>(())
```

## Implementing the library's interfaces

`whiteout::interfaces` holds the traits the library calls *into* — supply
your own file system, HTTP client, or thread pool:

```rust
use whiteout::interfaces::{FileSystem, HostFileSystem};

struct MyFs;
impl FileSystem for MyFs {
    fn read_file(&self, path: &str) -> Option<Vec<u8>> {
        std::fs::read(path).ok()
    }
}

let fs = HostFileSystem::new(MyFs);
```

`Send + Sync` is required, not defensive: the library calls these from
worker threads. Panics are contained at the boundary — the C ABI is
compiled without exceptions, so an escaping panic would be undefined
behaviour.

A pool can be handed to any call that takes one:

```rust
# use whiteout::interfaces::{HostWorkerPool, WorkerPool, WorkerTask};
# use whiteout::textures::{PixelFormat, Texture};
# struct Inline;
# impl WorkerPool for Inline {
#     fn submit(&self, task: WorkerTask) { task.run() }
#     fn wait_idle(&self) {}
#     fn thread_count(&self) -> usize { 4 }
# }
let pool = HostWorkerPool::new(Inline);
let tex = Texture::create_2d(PixelFormat::RGBA8, 64, 64, 1).unwrap();
let bc1 = tex.copy_as_format(PixelFormat::BC1, Some(&pool)).unwrap();
```

## Errors

The C++ library does not throw and reports absence via `std::optional`.
This binding follows suit: operations that can simply find nothing return
`Option`, and `Result` is reserved for the few calls that produce a real
diagnostic. Parser diagnostics are a list, not an error:

```rust
# use whiteout::textures::BlpParser;
let mut parser = BlpParser::new();
let texture = parser.parse(b"not a blp");     // -> None
for issue in parser.issues() {
    eprintln!("{issue}");
}
```

## Layout verification

Value types such as `math::Vector3f` are `#[repr(C)]` mirrors of their C++
counterparts and cross the boundary with no conversion. Sizes are pinned by
`const` assertions at compile time; to also check the library you linked:

```rust
whiteout::math::check_abi()?;
# Ok::<(), whiteout::Error>(())
```

## Regenerating

Everything except `interfaces.rs`, `support.rs` and `lib.rs` is generated.
Do not edit those files:

```sh
python -m tools.codegen.codegen textures --backend rust-abi-header
python -m tools.codegen.codegen textures --backend rust-abi-source
python -m tools.codegen.codegen textures --backend rust-math
python -m tools.codegen.codegen <module> --backend rust
```

## License

BSD-3-Clause.
