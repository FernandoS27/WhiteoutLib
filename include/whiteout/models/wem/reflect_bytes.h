// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file reflect_bytes.h
 * @brief Value comparison for anything with a `reflect()`.
 *
 * WEM's structs have no `operator==`, and most of them could not have a useful
 * one: a material's body is a variant, its native block another, and a
 * hand-written comparison is one more field list to keep in step with the
 * struct's own. `reflect()` already is that list, so this visitor walks it and
 * appends every value it names to a byte string. Equal bytes, equal value.
 *
 * It is not the file format. There is no chunk layout and no deferred write,
 * and every field is visited regardless of `since`, because the question is
 * what the object in memory holds, not what a given version would store.
 *
 * Every visitor call `reflect()` makes is implemented (`field`, `optional`,
 * `inlineList`, `count`, `chunk`, `chunkAlternative`, `since`). A struct whose
 * `reflect()` uses one a visitor lacks does not compile, so nothing can be
 * compared by a partial walk.
 *
 * Bytes, not `==`: `-0.0f` and `0.0f` differ, as they do in a saved file, and a
 * NaN equals itself, so a value that was NaN before an edit and is NaN after it
 * counts as unchanged.
 */

#include <whiteout/common_types.h>
#include <whiteout/models/wem/reflect.h>

#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

class ReflectBytesVisitor {
public:
    static constexpr bool kReading = false;

    std::vector<u8> bytes;

    template <class T>
    void field(const char*, T& value) {
        constexpr FieldKind kind = ClassifyField<T>();
        if constexpr (kind == FieldKind::Enum) {
            raw(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (kind == FieldKind::Bool) {
            bytes.push_back(value ? 1 : 0);
        } else if constexpr (kind == FieldKind::String) {
            raw(static_cast<u64>(value.size()));
            bytes.insert(bytes.end(), value.begin(), value.end());
        } else if constexpr (kind == FieldKind::Vector) {
            list(value);
        } else if constexpr (kind == FieldKind::Reflected) {
            value.reflect(*this);
        } else {
            raw(value);
        }
    }

    template <class T>
    void optional(const char* name, std::optional<T>& value) {
        bytes.push_back(value.has_value() ? 1 : 0);
        if (value.has_value()) {
            field(name, *value);
        }
    }

    template <class T>
    void inlineList(const char*, std::vector<T>& value) {
        list(value);
    }

    /// The length of a run whose entries `reflect()` then visits one by one.
    template <class C>
    void count(const char*, const C& container) {
        raw(static_cast<u64>(container.size()));
    }

    template <class T>
    void chunk(const char*, T& value) {
        value.reflect(*this);
    }

    /// The discriminator was already visited as a field; this adds the body.
    /// A block this build skipped on read leaves the variant empty, and the
    /// one byte is all that can be said about it.
    template <class Alt, class Variant>
    void chunkAlternative(const char*, Variant& value, u32&) {
        if (!std::holds_alternative<Alt>(value)) {
            bytes.push_back(0);
            return;
        }
        bytes.push_back(1);
        std::get<Alt>(value).reflect(*this);
    }

    ReflectBytesVisitor& since(u32) {
        return *this;
    }

private:
    template <class T>
    void list(std::vector<T>& value) {
        raw(static_cast<u64>(value.size()));
        if constexpr (ClassifyField<T>() == FieldKind::Raw) {
            // A run of positions or indices is one copy, not a call per entry:
            // a whole document's comparison is dominated by these.
            const auto* at = reinterpret_cast<const u8*>(value.data());
            bytes.insert(bytes.end(), at, at + value.size() * sizeof(T));
        } else {
            for (T& element : value) {
                field("", element);
            }
        }
    }

    /// Trivially copyable, and in WEM a run of `f32` or integers with no padding
    /// between them, so its bytes are its value.
    template <class T>
    void raw(const T& value) {
        const auto* at = reinterpret_cast<const u8*>(&value);
        bytes.insert(bytes.end(), at, at + sizeof(T));
    }
};

/// Everything @p value's `reflect()` names, as bytes. Two values with the same
/// bytes are the same value; the bytes also make a key for "rebuild when this
/// changed".
///
/// `reflect()` takes a mutable visitor target because the reader shares it;
/// a non-reading visitor writes nothing back, which is what makes the cast here
/// sound.
template <class T>
std::vector<u8> ReflectBytes(const T& value) {
    ReflectBytesVisitor visitor;
    visitor.field("", const_cast<T&>(value));
    return std::move(visitor.bytes);
}

/// Whether @p a and @p b hold the same values in every reflected field.
template <class T>
bool SameReflected(const T& a, const T& b) {
    return ReflectBytes(a) == ReflectBytes(b);
}

} // namespace wem
} // namespace models
} // namespace whiteout
