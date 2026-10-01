// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file ascii.cpp
 * @brief The ASCII FBX encoding, read into the same record tree as the binary.
 *
 * `Name: value, value, … { children }`, `;` comments, quoted strings with
 * `&quot;` for a quote, and arrays as `Name: *N { a: v, v, … }`. ASCII has no
 * value types, so a number with a radix point or exponent reads as f64 and any
 * other as i64; an array is f64 if any element is. Bare words (`T`, `Y`, `N`)
 * read as strings.
 */

#include "whiteout/models/fbx/fbx.h"

#include <cctype>

#include "../../common/text.h"

namespace whiteout {
namespace models {
namespace fbx {

namespace {

constexpr u32 kMaxDepth = 64;

enum class Token : u8 { End, Name, String, Number, Word, Comma, Open, Close, Star, Error };

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    Token next() {
        skip();
        start_ = pos_;
        if (pos_ >= text_.size()) {
            return Token::End;
        }
        const char c = text_[pos_];
        if (c == ',') {
            ++pos_;
            return Token::Comma;
        }
        if (c == '{') {
            ++pos_;
            return Token::Open;
        }
        if (c == '}') {
            ++pos_;
            return Token::Close;
        }
        if (c == '*') {
            ++pos_;
            return Token::Star;
        }
        if (c == '"') {
            ++pos_;
            const std::size_t close = text_.find('"', pos_);
            if (close == std::string_view::npos) {
                return Token::Error;
            }
            value_ = text_.substr(pos_, close - pos_);
            pos_ = close + 1;
            return Token::String;
        }
        if (c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) {
            while (pos_ < text_.size() && IsNumberChar(text_[pos_])) {
                ++pos_;
            }
            value_ = text_.substr(start_, pos_ - start_);
            return Token::Number;
        }
        if (IsWordChar(c)) {
            while (pos_ < text_.size() && IsWordChar(text_[pos_])) {
                ++pos_;
            }
            value_ = text_.substr(start_, pos_ - start_);
            if (pos_ < text_.size() && text_[pos_] == ':') {
                ++pos_;
                return Token::Name;
            }
            return Token::Word;
        }
        return Token::Error;
    }

    std::string_view value() const {
        return value_;
    }
    /// The line the current token starts on, for errors.
    u32 line() const {
        u32 lines = 1;
        for (std::size_t i = 0; i < start_ && i < text_.size(); ++i) {
            lines += text_[i] == '\n' ? 1u : 0u;
        }
        return lines;
    }

private:
    static bool IsNumberChar(char c) {
        return std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' ||
               c == 'e' || c == 'E';
    }
    static bool IsWordChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '|' || c == '-';
    }

    void skip() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (text::IsSpace(c)) {
                ++pos_;
            } else if (c == ';') {
                while (pos_ < text_.size() && text_[pos_] != '\n') {
                    ++pos_;
                }
            } else {
                break;
            }
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::size_t start_ = 0;
    std::string_view value_;
};

bool LooksFloating(std::string_view token) {
    for (const char c : token) {
        if (c == '.' || c == 'e' || c == 'E') {
            return true;
        }
    }
    return false;
}

std::string Unescape(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in.substr(i).starts_with("&quot;")) {
            out.push_back('"');
            i += 5;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

class AsciiParser {
public:
    explicit AsciiParser(std::string_view text) : lexer_(text) {}

    std::string error;

    bool parseFile(File& file) {
        token_ = lexer_.next();
        while (token_ != Token::End) {
            Node node;
            if (!parseNode(node, 0)) {
                return false;
            }
            file.nodes.push_back(std::move(node));
        }
        return true;
    }

private:
    bool fail(const std::string& message) {
        if (error.empty()) {
            error = message + " (line " + std::to_string(lexer_.line()) + ")";
        }
        return false;
    }

    bool value(Property& out) {
        switch (token_) {
        case Token::String:
            out = Property::Str(Unescape(lexer_.value()));
            break;
        case Token::Word:
            out = Property::Str(std::string(lexer_.value()));
            break;
        case Token::Number: {
            const std::string_view number = lexer_.value();
            if (LooksFloating(number)) {
                f64 parsed = 0.0;
                if (!text::ParseF64(number, parsed)) {
                    return fail("a number that does not parse");
                }
                out = Property::Double(parsed);
            } else {
                i64 parsed = 0;
                if (!text::ParseI64(number, parsed)) {
                    f64 wide = 0.0;
                    if (!text::ParseF64(number, wide)) {
                        return fail("a number that does not parse");
                    }
                    out = Property::Double(wide);
                } else {
                    out = Property::Long(parsed);
                }
            }
            break;
        }
        default:
            return fail("a value was expected");
        }
        token_ = lexer_.next();
        return true;
    }

    /// `*N { a: v, v, … }` once the star has been read.
    bool array(Property& out) {
        token_ = lexer_.next(); // the count, which the values say again
        if (token_ != Token::Number) {
            return fail("an array without its length");
        }
        token_ = lexer_.next();
        if (token_ != Token::Open) {
            return fail("an array without its block");
        }
        token_ = lexer_.next();
        std::vector<std::string_view> numbers;
        bool floating = false;
        if (token_ == Token::Name) {
            token_ = lexer_.next();
            while (token_ == Token::Number) {
                numbers.push_back(lexer_.value());
                floating = floating || LooksFloating(lexer_.value());
                token_ = lexer_.next();
                if (token_ != Token::Comma) {
                    break;
                }
                token_ = lexer_.next();
            }
        }
        if (token_ != Token::Close) {
            return fail("an array block that does not close");
        }
        token_ = lexer_.next();
        if (floating) {
            std::vector<f64> values(numbers.size());
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                if (!text::ParseF64(numbers[i], values[i])) {
                    return fail("an array element that does not parse");
                }
            }
            out = Property::Doubles(std::move(values));
        } else {
            std::vector<i64> values(numbers.size());
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                if (!text::ParseI64(numbers[i], values[i])) {
                    return fail("an array element that does not parse");
                }
            }
            out = Property::Longs(std::move(values));
        }
        return true;
    }

    bool parseNode(Node& node, u32 depth) {
        if (depth > kMaxDepth) {
            return fail("records nest deeper than any FBX writer goes");
        }
        if (token_ != Token::Name) {
            return fail("a record name was expected");
        }
        node.name = std::string(lexer_.value());
        token_ = lexer_.next();
        // The values, comma-separated; a value-less record goes straight to
        // its block or to the next record.
        if (token_ == Token::Star) {
            Property property;
            if (!array(property)) {
                return false;
            }
            node.properties.push_back(std::move(property));
        } else if (token_ == Token::String || token_ == Token::Word || token_ == Token::Number ||
                   token_ == Token::Comma) {
            for (;;) {
                if (token_ == Token::Comma) {
                    // An empty value, as `Content: , "…"` writes one.
                    node.properties.push_back(Property::Str(std::string()));
                    token_ = lexer_.next();
                    continue;
                }
                Property property;
                if (!value(property)) {
                    return false;
                }
                node.properties.push_back(std::move(property));
                if (token_ != Token::Comma) {
                    break;
                }
                token_ = lexer_.next();
            }
        }
        if (token_ == Token::Open) {
            node.block = true;
            token_ = lexer_.next();
            while (token_ != Token::Close) {
                if (token_ == Token::End) {
                    return fail("a block that does not close");
                }
                Node child;
                if (!parseNode(child, depth + 1)) {
                    return false;
                }
                node.children.push_back(std::move(child));
            }
            token_ = lexer_.next();
        }
        return true;
    }

    Lexer lexer_;
    Token token_ = Token::End;
};

} // namespace

ReadOutcome ReadAscii(std::string_view text) {
    ReadOutcome outcome;
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB &&
        static_cast<u8>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    File file;
    file.binary = false;
    AsciiParser parser(text);
    if (!parser.parseFile(file)) {
        outcome.error = parser.error;
        return outcome;
    }
    // The version is a record, not a header field.
    file.version = 0;
    if (const Node* header = file.find("FBXHeaderExtension")) {
        if (const Node* version = header->child("FBXVersion");
            version != nullptr && !version->properties.empty()) {
            file.version = static_cast<u32>(version->properties[0].toI64());
        }
    }
    if (file.version == 0) {
        outcome.warnings.push_back("no FBXVersion record; read as 7.4");
        file.version = 7400;
    }
    if (file.version < 7000) {
        outcome.error = "FBX " + std::to_string(file.version) +
                        " predates 7.0; files from before the 2011 SDK are not read";
        return outcome;
    }
    outcome.file = std::move(file);
    return outcome;
}

} // namespace fbx
} // namespace models
} // namespace whiteout
