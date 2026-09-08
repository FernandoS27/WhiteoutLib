// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file json.h
 * @brief A minimal JSON reader and writer, scoped to the glTF module.
 *
 * The library's policy is zero external codec dependencies, and a JSON reader is
 * the easiest codec on that list — small next to the in-house JPEG decoder. This
 * is deliberately not a general-purpose JSON library: it exists to read and
 * write glTF bodies, so it is exception-free, bounded (depth limit, number
 * grammar validated before conversion), and **deterministic on output** — object
 * members serialize in insertion order and floats print `%.9g`, which is what
 * makes an exported `.glb` byte-stable across runs and therefore golden-testable.
 *
 * Numbers are one shape, `f64`. Every count glTF can express fits it exactly
 * (indices are < 2^32 and 2^53 is the integral ceiling), and a writer that
 * prints integral values without a decimal point keeps validators and diffs
 * happy without a second storage class.
 */

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

namespace whiteout {
namespace models {
namespace gltf {
namespace json {

enum class Kind : u8 { Null, Bool, Number, String, Array, Object };

/// One object member; defined after `Value`, forward-declared here so the
/// value can hold a vector of them (incomplete element types are a documented
/// `std::vector` capability, and the same trick the array member uses on
/// `Value` itself).
struct Member;

/**
 * @brief One JSON value of any kind.
 *
 * Accessors are total: asking an array question of a number answers with the
 * empty/fallback value rather than failing, so decode code reads linearly and
 * checks presence with `find`/`kind` where absence matters.
 */
class Value {
public:
    Value() = default;

    static Value null();
    static Value boolean(bool value);
    static Value number(f64 value);
    static Value string(std::string value);
    static Value array();
    static Value object();

    Kind kind() const {
        return kind_;
    }
    bool isNull() const {
        return kind_ == Kind::Null;
    }
    bool isBool() const {
        return kind_ == Kind::Bool;
    }
    bool isNumber() const {
        return kind_ == Kind::Number;
    }
    bool isString() const {
        return kind_ == Kind::String;
    }
    bool isArray() const {
        return kind_ == Kind::Array;
    }
    bool isObject() const {
        return kind_ == Kind::Object;
    }

    bool asBool(bool fallback = false) const;
    f64 asNumber(f64 fallback = 0.0) const;
    /// Truncating integer views of a number, clamped at the type's range.
    u32 asU32(u32 fallback = 0) const;
    i64 asI64(i64 fallback = 0) const;
    f32 asF32(f32 fallback = 0.0f) const;
    const std::string& asString() const; ///< Empty string for a non-string.

    // --- arrays ---

    std::size_t size() const; ///< Elements or members; 0 for scalars.
    const Value& at(std::size_t index) const; ///< Null sentinel out of range.
    /// Appends to an array (converting a null to an array first) and returns
    /// the stored element.
    Value& push(Value element);

    // --- objects ---

    const Value* find(std::string_view key) const; ///< Null when absent.
    /// Sets (or replaces) a member, converting a null to an object first.
    /// Returns the stored value.
    Value& set(std::string_view key, Value value);
    const Member& memberAt(std::size_t index) const;

    // --- typed member conveniences, the decoder's vocabulary ---

    f64 numberOf(std::string_view key, f64 fallback = 0.0) const;
    u32 u32Of(std::string_view key, u32 fallback = 0) const;
    f32 f32Of(std::string_view key, f32 fallback = 0.0f) const;
    bool boolOf(std::string_view key, bool fallback = false) const;
    const std::string& stringOf(std::string_view key) const;
    const Value& arrayOf(std::string_view key) const; ///< Null sentinel when absent.

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    f64 number_ = 0.0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<Member> members_;
};

/// Order is insertion order and is preserved on both read and write —
/// determinism is a feature, and glTF tooling diffs JSON bodies.
struct Member {
    std::string key;
    Value value;
};

// ============================================================================
// Parse
// ============================================================================

struct ParseResult {
    std::optional<Value> value;
    std::string error;      ///< Empty on success.
    std::size_t offset = 0; ///< Byte offset the error was noticed at.

    bool ok() const {
        return value.has_value();
    }
};

/**
 * @brief Parses one JSON document. Strict: no comments, no trailing commas,
 *        no NaN/Infinity tokens, nothing after the root value but whitespace.
 *
 * @p maxDepth bounds container nesting — the parser is recursive descent, and a
 * hostile file of ten thousand `[` must fail cleanly, not overflow the stack.
 */
ParseResult Parse(std::string_view text, u32 maxDepth = 96);

// ============================================================================
// Write
// ============================================================================

/// Serializes @p value, deterministically: members in insertion order,
/// integral numbers without a decimal point, other numbers `%.9g`, strings
/// minimally escaped. No whitespace — glTF bodies are machine-read.
std::string Write(const Value& value);

/// The number spelling `Write` uses, exposed because tests golden it.
std::string FormatNumber(f64 value);

} // namespace json
} // namespace gltf
} // namespace models
} // namespace whiteout
