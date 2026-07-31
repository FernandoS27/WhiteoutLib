# whiteout

Rust bindings for [WhiteoutLib](https://github.com/Sahmkow/WhiteoutLib) —
parsers and writers for Blizzard model and texture formats (MDX, M2, M3,
BLP, DDS, PNG, …).

> **Status: in development.** The `math` module is complete and tested;
> textures and the model formats land in later phases. See
> [`docs/plans/rust-bindings.md`](../../../docs/plans/rust-bindings.md).

## Building

The crate links against `whiteout_native`, which is built by CMake:

```powershell
./scripts/build-rust.ps1              # codegen + cmake + fmt + clippy + test
./scripts/build-rust.ps1 -Static      # link whiteout_native_static instead
```

Manually:

```sh
cmake -S . -B build-rust -DWHITEOUT_BUILD_C_BINDINGS=ON
cmake --build build-rust --config Release --target whiteout_c
WHITEOUT_LIB_DIR=build-rust/c-dist/Release cargo test
```

`build.rs` resolves the library in this order: `WHITEOUT_LIB_DIR`, then
`pkg-config`. Set `WHITEOUT_STATIC=1` to link the static archive.

## Design

Value types are `#[repr(C)]` mirrors of the C++ types and cross the FFI
boundary with no conversion and no allocation:

```rust
use whiteout::math::{Matrix44f, Quaternion, Vector3f};

let a = Vector3f::new(1.0, 0.0, 0.0);
let b = Vector3f::new(0.0, 2.0, 0.0);
let c = (a + b) * 2.0 - a;          // no allocation, `a` still usable

let m = Matrix44f::compose(
    Vector3f::new(1.0, 2.0, 3.0),
    Quaternion::from_axis_angle(Vector3f::new(0.0, 1.0, 0.0), 0.7),
    Vector3f::new(2.0, 2.0, 2.0),
);
let inv = m * Matrix44f::inverse(m);   // ≈ identity
```

Component-wise arithmetic, `dot` and `length` are plain Rust — they never
call across the boundary. Operations with subtler semantics (the Hamilton
product, `slerp`, matrix inverse, the spline interpolators) call the C++
implementation, so this binding cannot drift from the library. The test
suite cross-checks every native implementation against its C++ counterpart.

### Layout verification

Sizes are pinned by `const` assertions at compile time. To also verify the
library you actually linked:

```rust
whiteout::math::check_abi()?;
```

### Errors

The C++ library does not throw and reports absence via `std::optional`.
This binding follows suit: operations that can simply find nothing return
`Option`, and `Result` is reserved for the few calls that produce a real
diagnostic.

## Regenerating

`src/math.rs` is generated. Do not edit it:

```sh
python -m tools.codegen.codegen textures --backend rust-math
```

## License

BSD-3-Clause.
