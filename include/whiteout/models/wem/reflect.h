// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file reflect.h
 * @brief The `reflect(V&)` visitor concept — §11.2.
 *
 * Every serializable struct declares its fields once, in serialization order:
 *
 * ```cpp
 * template <class V> void reflect(V& v) {
 *     v.field("name", name);
 *     v.field("bounds", bounds);
 *     v.since(2).field("selectionGroup", selectionGroup);
 * }
 * ```
 *
 * and that one declaration drives the binary reader, the binary writer, the text
 * dump and the structural hash. **It does not change the bytes**: the field kinds
 * below are exactly the cases the hand-written v2 visitors distinguished, in the
 * same order, into the same deferred-write queue. P4's parity test is the proof.
 *
 * Declaration order in the struct is *not* the contract — `reflect()`'s call
 * order is. The two already disagree in v2 (`Material::textureSlots` is declared
 * fifth and serialized last), and following the struct would have been a format
 * change disguised as a refactor.
 */

#include <whiteout/common_types.h>

#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Detecting a reflected type
// ============================================================================

/// A visitor that accepts every call and does nothing, used only to ask whether
/// a type has a `reflect()`. `decltype` does not instantiate the body, so this
/// costs a declaration check and not a full instantiation.
struct ProbeVisitor {
    static constexpr bool kReading = false;

    template <class T>
    void field(const char*, T&) {}
    template <class T>
    void optional(const char*, T&) {}
    template <class T>
    void inlineList(const char*, T&) {}
    template <class C>
    void count(const char*, C&) {}
    template <class T>
    void chunk(const char*, T&) {}
    template <class Alt, class Variant>
    void chunkAlternative(const char*, Variant&, u32&) {}

    ProbeVisitor& since(u32) {

        return *this;
    }
};

template <class T, class = void>
inline constexpr bool kHasReflect = false;

template <class T>
inline constexpr bool kHasReflect<
    T, std::void_t<decltype(std::declval<T&>().reflect(std::declval<ProbeVisitor&>()))>> = true;

template <class T>
inline constexpr bool kIsVector = false;

template <class T, class A>
inline constexpr bool kIsVector<std::vector<T, A>> = true;

template <class T>
inline constexpr bool kIsOptional = false;

template <class T>
inline constexpr bool kIsOptional<std::optional<T>> = true;

/// How one `v.field()` serializes. Ordered: the first case that matches wins,
/// which is why a reflected-and-trivially-copyable struct (`FresnelProperties`)
/// takes the reflected path rather than becoming one memcpy — the two happen to
/// agree byte for byte today, and pinning the reflected reading keeps them
/// agreeing when a field is added.
enum class FieldKind {
    Enum,      ///< written as its **underlying** type, not always u32
    Bool,      ///< one `u8`, 0 or 1
    String,    ///< inline `Reference` to a `CHAR` chunk, not NUL-terminated
    Vector,    ///< inline `Reference` to a `ChunkTagTraits<T>` chunk
    Reflected, ///< recursed inline, no chunk of its own
    Raw,       ///< trivially copyable, memcpy'd inline
};

template <class T>
constexpr FieldKind ClassifyField() {
    if constexpr (std::is_enum_v<T>) {
        return FieldKind::Enum;
    } else if constexpr (std::is_same_v<T, bool>) {
        return FieldKind::Bool;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return FieldKind::String;
    } else if constexpr (kIsVector<T>) {
        return FieldKind::Vector;
    } else if constexpr (kHasReflect<T>) {
        return FieldKind::Reflected;
    } else {
        static_assert(std::is_trivially_copyable_v<T>,
                      "a reflected field is an enum, a bool, a string, a vector, another "
                      "reflected struct, or trivially copyable — nothing else has a defined "
                      "encoding");
        return FieldKind::Raw;
    }
}

// ============================================================================
// Variants
// ============================================================================

/// A variant is serialized as its discriminator followed by the fields of the
/// alternative that discriminator names. Both halves need the same two steps —
/// settle the kind, then reach the alternative — so they are written once here
/// rather than four times across the material, node, texture and attribute
/// bodies.

/// Visits @p value's discriminator under @p name and returns it. On write that
/// emits what the variant already holds; on read it is the byte that decides
/// which `VariantAs` below is legal.
template <class Kind, class V, class Variant>
Kind VariantKind(V& v, const char* name, Variant& value) {
    Kind kind = static_cast<Kind>(value.index());
    v.field(name, kind);
    return kind;
}

/// The alternative @p Alt of @p value, constructing it first if the variant is
/// not already holding it. Writing never constructs — the variant holds what it
/// holds; reading always does, because the alternative is what the discriminator
/// just said and not what the default-constructed variant happens to be.
template <class Alt, class Variant>
Alt& VariantAs(Variant& value) {
    if (!std::holds_alternative<Alt>(value)) {
        value.template emplace<Alt>();
    }
    return std::get<Alt>(value);
}

} // namespace wem
} // namespace models
} // namespace whiteout
