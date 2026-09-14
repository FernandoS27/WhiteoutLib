// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/gltf/json.h"

#include <cerrno>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace whiteout {
namespace models {
namespace gltf {
namespace json {

// ============================================================================
// Value
// ============================================================================

Value Value::null() {
    return Value{};
}

Value Value::boolean(bool value) {
    Value v;
    v.kind_ = Kind::Bool;
    v.bool_ = value;
    return v;
}

Value Value::number(f64 value) {
    Value v;
    v.kind_ = Kind::Number;
    v.number_ = value;
    return v;
}

Value Value::string(std::string value) {
    Value v;
    v.kind_ = Kind::String;
    v.string_ = std::move(value);
    return v;
}

Value Value::array() {
    Value v;
    v.kind_ = Kind::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.kind_ = Kind::Object;
    return v;
}

namespace {
const Value kNullSentinel{};
const std::string kEmptyString{};
} // namespace

bool Value::asBool(bool fallback) const {
    return kind_ == Kind::Bool ? bool_ : fallback;
}

f64 Value::asNumber(f64 fallback) const {
    return kind_ == Kind::Number ? number_ : fallback;
}

u32 Value::asU32(u32 fallback) const {
    if (kind_ != Kind::Number || !(number_ >= 0.0) || number_ >= 4294967296.0) {
        return fallback;
    }
    return static_cast<u32>(number_);
}

i64 Value::asI64(i64 fallback) const {
    if (kind_ != Kind::Number || std::isnan(number_) || number_ <= -9.3e18 || number_ >= 9.3e18) {
        return fallback;
    }
    return static_cast<i64>(number_);
}

f32 Value::asF32(f32 fallback) const {
    return kind_ == Kind::Number ? static_cast<f32>(number_) : fallback;
}

const std::string& Value::asString() const {
    return kind_ == Kind::String ? string_ : kEmptyString;
}

std::size_t Value::size() const {
    if (kind_ == Kind::Array) {
        return array_.size();
    }
    if (kind_ == Kind::Object) {
        return members_.size();
    }
    return 0;
}

const Value& Value::at(std::size_t index) const {
    if (kind_ != Kind::Array || index >= array_.size()) {
        return kNullSentinel;
    }
    return array_[index];
}

Value& Value::push(Value element) {
    if (kind_ != Kind::Array) {
        *this = array();
    }
    array_.push_back(std::move(element));
    return array_.back();
}

const Value* Value::find(std::string_view key) const {
    if (kind_ != Kind::Object) {
        return nullptr;
    }
    for (const Member& member : members_) {
        if (member.key == key) {
            return &member.value;
        }
    }
    return nullptr;
}

Value& Value::set(std::string_view key, Value value) {
    if (kind_ != Kind::Object) {
        *this = object();
    }
    for (Member& member : members_) {
        if (member.key == key) {
            member.value = std::move(value);
            return member.value;
        }
    }
    members_.push_back(Member{std::string(key), std::move(value)});
    return members_.back().value;
}

const Member& Value::memberAt(std::size_t index) const {
    static const Member kNullMember{};
    if (kind_ != Kind::Object || index >= members_.size()) {
        return kNullMember;
    }
    return members_[index];
}

f64 Value::numberOf(std::string_view key, f64 fallback) const {
    const Value* found = find(key);
    return found != nullptr ? found->asNumber(fallback) : fallback;
}

u32 Value::u32Of(std::string_view key, u32 fallback) const {
    const Value* found = find(key);
    return found != nullptr ? found->asU32(fallback) : fallback;
}

f32 Value::f32Of(std::string_view key, f32 fallback) const {
    const Value* found = find(key);
    return found != nullptr ? found->asF32(fallback) : fallback;
}

bool Value::boolOf(std::string_view key, bool fallback) const {
    const Value* found = find(key);
    return found != nullptr ? found->asBool(fallback) : fallback;
}

const std::string& Value::stringOf(std::string_view key) const {
    const Value* found = find(key);
    return found != nullptr ? found->asString() : kEmptyString;
}

const Value& Value::arrayOf(std::string_view key) const {
    const Value* found = find(key);
    return (found != nullptr && found->isArray()) ? *found : kNullSentinel;
}

// ============================================================================
// Parse
// ============================================================================

namespace {

struct Reader {
    std::string_view text;
    std::size_t pos = 0;
    u32 maxDepth = 0;
    std::string error;
    std::string numberScratch;

    /// `std::from_chars` for f64. libc++ before LLVM 20 — emsdk's pinned
    /// toolchain and AppleClang — ships only the integral overloads, so fall
    /// back to `strtod` there. Callers grammar-check the token first, so
    /// `strtod` cannot widen what the parser accepts; it does honour
    /// LC_NUMERIC, hence the radix substitution.
    std::from_chars_result toDouble(const char* first, const char* last, f64& out) {
#if defined(__cpp_lib_to_chars)
        return std::from_chars(first, last, out);
#else
        numberScratch.assign(first, last);
        const char* const radix = std::localeconv()->decimal_point;
        if (std::strcmp(radix, ".") != 0) {
            const std::size_t dot = numberScratch.find('.');
            if (dot != std::string::npos) {
                numberScratch.replace(dot, 1, radix);
            }
        }
        const char* const buffer = numberScratch.c_str();
        char* end = nullptr;
        errno = 0;
        out = std::strtod(buffer, &end);
        // strtod raises ERANGE for subnormal results as well, but from_chars
        // calls those a success — they are representable. Only a magnitude
        // that saturated to infinity or collapsed to zero is out of range.
        const bool unrepresentable = errno == ERANGE && (out == 0.0 || std::isinf(out));
        std::from_chars_result result;
        result.ptr = first + (end - buffer);
        result.ec = unrepresentable ? std::errc::result_out_of_range : std::errc();
        return result;
#endif
    }

    bool fail(const char* message) {
        if (error.empty()) {
            error = message;
        }
        return false;
    }

    bool atEnd() const {
        return pos >= text.size();
    }

    char peek() const {
        return text[pos];
    }

    void skipWhitespace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos;
        }
    }

    bool consume(char expected, const char* message) {
        skipWhitespace();
        if (atEnd() || text[pos] != expected) {
            return fail(message);
        }
        ++pos;
        return true;
    }

    bool literal(std::string_view word) {
        if (text.size() - pos < word.size() ||
            text.compare(pos, word.size(), word) != 0) {
            return fail("unrecognized token");
        }
        pos += word.size();
        return true;
    }

    static void appendUtf8(std::string& out, u32 codepoint) {
        if (codepoint < 0x80) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool hex4(u32& out) {
        if (text.size() - pos < 4) {
            return fail("truncated \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[pos++];
            u32 digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<u32>(c - 'a') + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<u32>(c - 'A') + 10;
            } else {
                return fail("bad hex digit in \\u escape");
            }
            out = (out << 4) | digit;
        }
        return true;
    }

    bool parseString(std::string& out) {
        // Caller consumed the opening quote.
        out.clear();
        while (true) {
            if (atEnd()) {
                return fail("unterminated string");
            }
            const char c = text[pos++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("unescaped control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (atEnd()) {
                return fail("truncated escape");
            }
            const char esc = text[pos++];
            switch (esc) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                u32 code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code <= 0xDBFF) {
                    // High surrogate — a low surrogate escape must follow.
                    if (text.size() - pos < 2 || text[pos] != '\\' || text[pos + 1] != 'u') {
                        return fail("lone high surrogate");
                    }
                    pos += 2;
                    u32 low = 0;
                    if (!hex4(low)) {
                        return false;
                    }
                    if (low < 0xDC00 || low > 0xDFFF) {
                        return fail("bad low surrogate");
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    return fail("lone low surrogate");
                }
                appendUtf8(out, code);
                break;
            }
            default:
                return fail("unrecognized escape");
            }
        }
    }

    /// Validates the token against the JSON number grammar before conversion,
    /// because `std::from_chars` accepts spellings JSON forbids ("inf", "nan",
    /// a leading '+', a bare '.5').
    bool parseNumber(f64& out) {
        const std::size_t start = pos;
        if (!atEnd() && text[pos] == '-') {
            ++pos;
        }
        if (atEnd() || text[pos] < '0' || text[pos] > '9') {
            return fail("malformed number");
        }
        if (text[pos] == '0') {
            ++pos;
        } else {
            while (!atEnd() && text[pos] >= '0' && text[pos] <= '9') {
                ++pos;
            }
        }
        if (!atEnd() && text[pos] == '.') {
            ++pos;
            if (atEnd() || text[pos] < '0' || text[pos] > '9') {
                return fail("malformed number");
            }
            while (!atEnd() && text[pos] >= '0' && text[pos] <= '9') {
                ++pos;
            }
        }
        if (!atEnd() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (!atEnd() && (text[pos] == '+' || text[pos] == '-')) {
                ++pos;
            }
            if (atEnd() || text[pos] < '0' || text[pos] > '9') {
                return fail("malformed number");
            }
            while (!atEnd() && text[pos] >= '0' && text[pos] <= '9') {
                ++pos;
            }
        }
        const char* first = text.data() + start;
        const char* last = text.data() + pos;
        const std::from_chars_result result = toDouble(first, last, out);
        if (result.ec == std::errc::result_out_of_range) {
            // A magnitude past f64 is data the file said; the nearest
            // representable answer (±inf collapses to ±HUGE) would poison
            // downstream math, so it is a parse error like any other.
            return fail("number out of range");
        }
        if (result.ec != std::errc() || result.ptr != last) {
            return fail("malformed number");
        }
        return true;
    }

    bool parseValue(Value& out, u32 depth) {
        if (depth == 0) {
            return fail("nesting too deep");
        }
        skipWhitespace();
        if (atEnd()) {
            return fail("unexpected end of input");
        }
        const char c = peek();
        switch (c) {
        case '{': {
            ++pos;
            out = Value::object();
            skipWhitespace();
            if (!atEnd() && peek() == '}') {
                ++pos;
                return true;
            }
            while (true) {
                if (!consume('"', "expected member name")) {
                    return false;
                }
                std::string key;
                if (!parseString(key)) {
                    return false;
                }
                if (!consume(':', "expected ':'")) {
                    return false;
                }
                Value member;
                if (!parseValue(member, depth - 1)) {
                    return false;
                }
                out.set(key, std::move(member));
                skipWhitespace();
                if (atEnd()) {
                    return fail("unterminated object");
                }
                if (peek() == ',') {
                    ++pos;
                    continue;
                }
                if (peek() == '}') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        case '[': {
            ++pos;
            out = Value::array();
            skipWhitespace();
            if (!atEnd() && peek() == ']') {
                ++pos;
                return true;
            }
            while (true) {
                Value element;
                if (!parseValue(element, depth - 1)) {
                    return false;
                }
                out.push(std::move(element));
                skipWhitespace();
                if (atEnd()) {
                    return fail("unterminated array");
                }
                if (peek() == ',') {
                    ++pos;
                    continue;
                }
                if (peek() == ']') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        case '"': {
            ++pos;
            std::string value;
            if (!parseString(value)) {
                return false;
            }
            out = Value::string(std::move(value));
            return true;
        }
        case 't':
            if (!literal("true")) {
                return false;
            }
            out = Value::boolean(true);
            return true;
        case 'f':
            if (!literal("false")) {
                return false;
            }
            out = Value::boolean(false);
            return true;
        case 'n':
            if (!literal("null")) {
                return false;
            }
            out = Value::null();
            return true;
        default: {
            f64 number = 0.0;
            if (!parseNumber(number)) {
                return false;
            }
            out = Value::number(number);
            return true;
        }
        }
    }
};

} // namespace

ParseResult Parse(std::string_view text, u32 maxDepth) {
    ParseResult result;
    Reader reader;
    reader.text = text;
    reader.maxDepth = maxDepth;

    Value root;
    if (!reader.parseValue(root, maxDepth)) {
        result.error = reader.error;
        result.offset = reader.pos;
        return result;
    }
    reader.skipWhitespace();
    if (!reader.atEnd()) {
        result.error = "trailing content after the root value";
        result.offset = reader.pos;
        return result;
    }
    result.value = std::move(root);
    return result;
}

// ============================================================================
// Write
// ============================================================================

std::string FormatNumber(f64 value) {
    if (std::isnan(value) || std::isinf(value)) {
        // No JSON spelling exists; zero is the least-wrong stand-in, and the
        // exporter never produces one on purpose.
        return "0";
    }
    if (value == std::floor(value) && std::fabs(value) < 9.007199254740992e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        return buffer;
    }
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return buffer;
}

namespace {

/// Bytes of the well-formed UTF-8 sequence starting at @p i, or 0.
std::size_t utf8SequenceLength(const std::string& text, std::size_t i) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::size_t length = 0;
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) {
        length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
    } else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4) {
        length = 4;
    } else {
        return 0;
    }
    if (i + length > text.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k) {
        if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
            return 0;
        }
    }
    return length;
}

void writeEscaped(std::string& out, const std::string& text) {
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        const unsigned char byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            ++i;
            continue;
        case '\\':
            out += "\\\\";
            ++i;
            continue;
        case '\b':
            out += "\\b";
            ++i;
            continue;
        case '\f':
            out += "\\f";
            ++i;
            continue;
        case '\n':
            out += "\\n";
            ++i;
            continue;
        case '\r':
            out += "\\r";
            ++i;
            continue;
        case '\t':
            out += "\\t";
            ++i;
            continue;
        default:
            break;
        }
        if (byte < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
            out += buffer;
            ++i;
            continue;
        }
        // JSON text must be valid UTF-8, and source-format strings are raw
        // bytes in whatever code page the author used — Warcraft III's corpus
        // is full of GB2312 names. A malformed byte becomes U+FFFD, one per
        // byte, so the output is always a legal document and always the same
        // one.
        const std::size_t length = utf8SequenceLength(text, i);
        if (length == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(text, i, length);
            i += length;
        }
    }
    out.push_back('"');
}

void writeValue(std::string& out, const Value& value) {
    switch (value.kind()) {
    case Kind::Null:
        out += "null";
        break;
    case Kind::Bool:
        out += value.asBool() ? "true" : "false";
        break;
    case Kind::Number:
        out += FormatNumber(value.asNumber());
        break;
    case Kind::String:
        writeEscaped(out, value.asString());
        break;
    case Kind::Array: {
        out.push_back('[');
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            writeValue(out, value.at(i));
        }
        out.push_back(']');
        break;
    }
    case Kind::Object: {
        out.push_back('{');
        for (std::size_t i = 0; i < value.size(); ++i) {
            const Member& member = value.memberAt(i);
            if (i != 0) {
                out.push_back(',');
            }
            writeEscaped(out, member.key);
            out.push_back(':');
            writeValue(out, member.value);
        }
        out.push_back('}');
        break;
    }
    }
}

} // namespace

std::string Write(const Value& value) {
    std::string out;
    writeValue(out, value);
    return out;
}

} // namespace json
} // namespace gltf
} // namespace models
} // namespace whiteout
