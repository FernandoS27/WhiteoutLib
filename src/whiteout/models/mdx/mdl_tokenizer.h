// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/common_types.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace whiteout {
namespace mdx {

enum class MdlTokenType : u8 {
    Number,       // integer or floating-point literal (e.g. 42, -3.14, .5, 1e-3)
    Identifier,   // alphanumeric word: [a-zA-Z_][a-zA-Z0-9_]*
    String,       // quoted string literal (content between the quotes)
    OpenBrace,    // {
    CloseBrace,   // }
    OpenBracket,  // [
    CloseBracket, // ]
    Comma,        // ,
    Colon,        // :
    Semicolon,    // ;
    Dot,          // . (standalone, not part of a number)
    EndOfFile,
    Error,
};

struct MdlToken {
    MdlTokenType type;
    std::string_view text; // view into the original source buffer
    u32 line;              // 1-based line number
    u32 column;            // 1-based column number
};

class MdlTokenizer {
public:
    // Tokenize the entire source buffer at once. Returns a flat token list
    // terminated by an EndOfFile token. The source data must outlive the
    // returned tokens (string_views point into it).
    static std::vector<MdlToken> tokenize(std::string_view source);

private:
    MdlTokenizer(std::string_view source);

    void run();
    void skipWhitespaceAndComments();
    void readNumber();
    void readIdentifier();
    void readString();

    std::string_view m_source;
    const char* m_cursor;
    const char* m_end;
    const char* m_lineStart; // pointer to the start of the current line
    u32 m_line;
    std::vector<MdlToken> m_tokens;
};

} // namespace mdx
} // namespace whiteout
