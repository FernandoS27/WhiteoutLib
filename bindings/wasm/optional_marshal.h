// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Embind ⇄ std::optional<T> bridge.
//
// Goal: a generated `.function("foo", &Class::foo)` whose return is
// `std::optional<T>` surfaces to JS as `null | T`, idiomatically.
//
// Emscripten ≥ 3.x already supplies `BindingType<std::optional<T>>` (see
// <emscripten/val.h>) and handles the generic case by mapping to JS
// `undefined | val(T)`. We layer two things on top:
//
//   1. A full specialisation for `std::optional<std::vector<u8>>` that
//      hands JS a *copied* Uint8Array (rather than the VectorU8 class
//      wrapper that `val(vector<u8>)` would otherwise produce). This is
//      the natural idiom for `readFile`-style APIs.
//
//   2. A `to_optional_val` helper used by the codegen for any method
//      returning `std::optional<MoveOnly>` (e.g. `mpq::Storage::open`).
//      Upstream's generic does `val(*value)` which requires a copy ctor;
//      our helper does `val(std::move(*value))` so move-only classes
//      survive the round-trip.
//
// Include before the `EMSCRIPTEN_BINDINGS(...)` block of any TU that
// uses either facility.

#pragma once

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace emscripten::internal {

// `null | Uint8Array` for byte-buffer returns. Full specialisation: wins
// over upstream's `BindingType<std::optional<T>>` partial template.
template <>
struct BindingType<std::optional<std::vector<std::uint8_t>>> {
    using WireType = typename BindingType<val>::WireType;

    template <typename ReturnPolicy = void>
    static WireType toWireType(std::optional<std::vector<std::uint8_t>> value,
                               rvp::default_tag tag) {
        if (!value.has_value()) {
            return BindingType<val>::toWireType(val::null(), tag);
        }
        // `new Uint8Array(typedMemoryView)` copies the bytes on the JS
        // side, so the returned array survives WASM memory growth.
        val u8arr = val::global("Uint8Array").new_(
            typed_memory_view(value->size(), value->data()));
        return BindingType<val>::toWireType(u8arr, tag);
    }

    static std::optional<std::vector<std::uint8_t>> fromWireType(WireType wt) {
        val js = BindingType<val>::fromWireType(wt);
        if (js.isNull() || js.isUndefined()) return std::nullopt;
        return convertJSArrayToNumberVector<std::uint8_t>(js);
    }
};

} // namespace emscripten::internal


namespace whiteout::wasm {

/// Convert a `std::optional<T>` to a heap-allocated `T*` (nullptr when
/// empty). The codegen pairs this with Embind's `allow_raw_pointers()`
/// policy so JS sees `null | T` — and the lambda return is a `T*` rather
/// than a value-typed T, which avoids Embind's internal copy-construction
/// at the wire-marshalling boundary (a problem for move-only classes
/// like `mpq::Storage`).
///
/// Ownership transfers to JS: the JS-side wrapper owns the heap T and
/// the user must call `.delete()` on it. This matches the conventional
/// Embind lifecycle for `class_<>`-bound types.
template <typename T>
T* to_optional_ptr(std::optional<T> value) {
    if (!value.has_value()) return nullptr;
    return new T(std::move(*value));
}

/// Move a prvalue `T` onto the heap and hand JS ownership of the pointer.
/// Used for non-optional move-only class returns (e.g. `Storage::create`).
template <typename T>
T* to_heap_ptr(T value) {
    return new T(std::move(value));
}

} // namespace whiteout::wasm
