// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file text_dump.h
 * @brief A readable dump of anything with a `reflect()` — §11.2, §16.
 *
 * §16's first testing note is that a green corpus gate proves a field parsed and
 * never that its value is right, and that the cheapest fix is a text dump diffed
 * against a recorded golden. This is that dump. It is also what makes a failed
 * byte comparison diagnosable: two files that differ at byte 4,912 tell you
 * nothing, and two dumps that differ on one line tell you everything.
 *
 * The output is deliberately not JSON. It is line-oriented and indentation-keyed
 * so that `diff` is the tool, and floats print with `%.9g` — enough digits to
 * round-trip an `f32` exactly, so a golden never hides a one-ulp change.
 */

#include <whiteout/models/wem/reflect.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

class TextDumpVisitor {
public:
    explicit TextDumpVisitor(std::ostream& out) : out_(out) {}
    static constexpr bool kReading = false;

    template <class T>
    void field(const char* name, T& value) {
        constexpr FieldKind kind = ClassifyField<T>();
        if constexpr (kind == FieldKind::Enum) {
            line(name, std::to_string(static_cast<std::int64_t>(
                           static_cast<std::underlying_type_t<T>>(value))));
        } else if constexpr (kind == FieldKind::Bool) {
            line(name, value ? "true" : "false");
        } else if constexpr (kind == FieldKind::String) {
            line(name, quote(value));
        } else if constexpr (kind == FieldKind::Vector) {
            dumpVector(name, value);
        } else if constexpr (kind == FieldKind::Reflected) {
            open(name, "");
            value.reflect(*this);
            close();
        } else {
            line(name, raw(value));
        }
    }

    template <class T>
    void optional(const char* name, std::optional<T>& value) {
        if (!value.has_value()) {
            line(name, "absent");
            return;
        }
        field(name, *value);
    }

    template <class T>
    void inlineList(const char* name, std::vector<T>& value) {
        open(name, "[" + std::to_string(value.size()) + "]");
        for (T& element : value) {
            field("", element);
        }
        close();
    }

    template <class Alt, class Variant>
    void chunkAlternative(const char* name, Variant& value, u32& slot) {
        if (!std::holds_alternative<Alt>(value)) {
            line(name, "skipped, preserved at slot " + std::to_string(slot));
            return;
        }
        chunk(name, std::get<Alt>(value));
    }

    template <class T>
    void chunk(const char* name, T& value) {
        open(name, "");
        value.reflect(*this);
        close();
    }

    template <class C>
    void count(const char* name, const C& container) {
        line(name, "[" + std::to_string(container.size()) + "]");
    }

    /// Every field is dumped regardless of chunk version — the dump describes
    /// the object in memory, not the bytes it came from.
    TextDumpVisitor& since(u32) {
        return *this;
    }

private:
    template <class T>
    void dumpVector(const char* name, std::vector<T>& value) {
        if constexpr (kHasReflect<T>) {
            open(name, "[" + std::to_string(value.size()) + "]");
            for (T& element : value) {
                open("", "");
                element.reflect(*this);
                close();
            }
            close();
        } else {
            // A run of trivial values is summarised, not listed: dumping 200k
            // positions defeats the purpose of a diffable dump. The count and
            // the first few are what a structural difference shows up in.
            std::string text = "[" + std::to_string(value.size()) + "]";
            const std::size_t shown = value.size() < 4 ? value.size() : 4;
            for (std::size_t i = 0; i < shown; ++i) {
                text += " " + raw(value[i]);
            }
            if (shown < value.size()) {
                text += " ...";
            }
            line(name, text);
        }
    }

    /// Trivially copyable and not otherwise classified: printed as the `f32`s or
    /// bytes it is made of, which covers every vector, quaternion and `Extent`
    /// without naming any of them.
    template <class T>
    static std::string raw(const T& value);

    static std::string quote(const std::string& text);
    void line(const char* name, const std::string& value);
    void open(const char* name, const std::string& suffix);
    void close();

    std::ostream& out_;
    int depth_ = 0;
};

namespace detail {

template <class T>
inline constexpr bool kIsByteArray = false;

template <std::size_t N>
inline constexpr bool kIsByteArray<std::array<u8, N>> = true;

/// Nine significant digits is what round-trips an `f32` exactly. A dump that
/// prints fewer is a golden that cannot see a one-ulp change.
inline std::string Real(f32 value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}

inline std::string Hex(const void* data, std::size_t size) {
    static const char* kDigits = "0123456789abcdef";
    const auto* bytes = static_cast<const u8*>(data);
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0x0F]);
    }
    return out;
}

} // namespace detail

template <class T>
std::string TextDumpVisitor::raw(const T& value) {
    if constexpr (std::is_floating_point_v<T>) {
        return detail::Real(static_cast<f32>(value));
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string(static_cast<std::int64_t>(value));
    } else if constexpr (detail::kIsByteArray<T>) {
        return detail::Hex(&value, sizeof(T));
    } else if constexpr (sizeof(T) % sizeof(f32) == 0) {
        // Every remaining trivially copyable type WEM serializes is a run of
        // `f32` -- the vectors, the quaternion, `Extent`. Reading them as one
        // covers all of them without the dump having to name any.
        f32 parts[sizeof(T) / sizeof(f32)];
        std::memcpy(parts, &value, sizeof(T));
        std::string out;
        for (const f32 part : parts) {
            out += (out.empty() ? "" : " ");
            out += detail::Real(part);
        }
        return out;
    } else {
        return detail::Hex(&value, sizeof(T));
    }
}

/// Dumps @p value under @p name. The entry point every caller wants.
template <class T>
void TextDump(std::ostream& out, const char* name, T& value) {
    TextDumpVisitor visitor(out);
    visitor.field(name, value);
}

} // namespace wem
} // namespace models
} // namespace whiteout
