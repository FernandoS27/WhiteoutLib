// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdl_tokenizer.h"

#include <cstring>

namespace whiteout {
namespace mdx {

// ─── Character classification lookup table ───────────────────────────────────
// Avoids per-character branching in the hot loop. Built once at static init.

namespace {

enum CharClass : u8 {
    CC_OTHER      = 0,        // anything not in the categories below
    CC_SPACE      = 1 << 0,   // whitespace (space, tab, \r)
    CC_NEWLINE    = 1 << 1,   // \n
    CC_DIGIT      = 1 << 2,   // 0-9
    CC_ALPHA      = 1 << 3,   // a-z A-Z _
    CC_DOT        = 1 << 4,   // .
    CC_SLASH      = 1 << 5,   // /
    CC_QUOTE      = 1 << 6,   // "
    CC_SIGN       = 1 << 7,   // + -  (for number sign prefix)
};

struct CharLUT {
    u8 table[256];

    constexpr CharLUT() : table{} {
        for (int i = 0; i < 256; ++i)
            table[i] = CC_OTHER;

        table[static_cast<u8>(' ')]  = CC_SPACE;
        table[static_cast<u8>('\t')] = CC_SPACE;
        table[static_cast<u8>('\r')] = CC_SPACE;
        table[static_cast<u8>('\n')] = CC_NEWLINE;

        for (int i = '0'; i <= '9'; ++i)
            table[i] = CC_DIGIT;

        for (int i = 'a'; i <= 'z'; ++i)
            table[i] = CC_ALPHA;
        for (int i = 'A'; i <= 'Z'; ++i)
            table[i] = CC_ALPHA;
        table[static_cast<u8>('_')] = CC_ALPHA;

        table[static_cast<u8>('.')] = CC_DOT;
        table[static_cast<u8>('/')] = CC_SLASH;
        table[static_cast<u8>('"')] = CC_QUOTE;
        table[static_cast<u8>('+')] = CC_SIGN;
        table[static_cast<u8>('-')] = CC_SIGN;
    }
};

constexpr CharLUT kLUT{};

inline u8 classify(char c) {
    return kLUT.table[static_cast<u8>(c)];
}

inline bool isDigit(char c)    { return (classify(c) & CC_DIGIT) != 0; }
[[maybe_unused]] inline bool isAlpha(char c)    { return (classify(c) & CC_ALPHA) != 0; }
inline bool isAlphaNum(char c) { return (classify(c) & (CC_ALPHA | CC_DIGIT)) != 0; }

} // anonymous namespace

// ─── MdlTokenizer implementation ────────────────────────────────────────────

MdlTokenizer::MdlTokenizer(std::string_view source)
    : m_source(source)
    , m_cursor(source.data())
    , m_end(source.data() + source.size())
    , m_lineStart(source.data())
    , m_line(1) {
    // Estimate ~1 token per 6 chars as a reasonable pre-allocation.
    m_tokens.reserve(source.size() / 6 + 64);
}

std::vector<MdlToken> MdlTokenizer::tokenize(std::string_view source) {
    MdlTokenizer tok(source);
    tok.run();
    return std::move(tok.m_tokens);
}

void MdlTokenizer::run() {
    while (m_cursor < m_end) {
        skipWhitespaceAndComments();
        if (m_cursor >= m_end)
            break;

        const char ch = *m_cursor;
        const u8 cls = classify(ch);

        const u32 col = static_cast<u32>(m_cursor - m_lineStart) + 1;

        // ── Single-character symbols ──
        switch (ch) {
        case '{':
            m_tokens.push_back({MdlTokenType::OpenBrace, {m_cursor, 1}, m_line, col});
            ++m_cursor;
            continue;
        case '}':
            m_tokens.push_back({MdlTokenType::CloseBrace, {m_cursor, 1}, m_line, col});
            ++m_cursor;
            continue;
        case '[':
            m_tokens.push_back({MdlTokenType::OpenBracket, {m_cursor, 1}, m_line, col});
            ++m_cursor;
            continue;
        case ']':
            m_tokens.push_back({MdlTokenType::CloseBracket, {m_cursor, 1}, m_line, col});
            ++m_cursor;
            continue;
        case ',':
            m_tokens.push_back({MdlTokenType::Comma, {m_cursor, 1}, m_line, col});
            ++m_cursor;
            continue;
        case ':':
            m_tokens.push_back({MdlTokenType::Colon, {m_cursor, 1}, m_line, col});
            ++m_cursor;
            continue;
        case ';':
            m_tokens.push_back({MdlTokenType::Semicolon, {m_cursor, 1}, m_line, col});
            ++m_cursor;
            continue;
        default:
            break;
        }

        // ── Numbers ──
        // Starts with digit, or '.' followed by digit, or sign (+/-) followed by digit or '.'
        if (cls & CC_DIGIT) {
            readNumber();
            continue;
        }
        if ((cls & CC_DOT) && (m_cursor + 1 < m_end) && isDigit(m_cursor[1])) {
            readNumber();
            continue;
        }
        if ((cls & CC_SIGN) && (m_cursor + 1 < m_end)) {
            const char next = m_cursor[1];
            if (isDigit(next) || (next == '.' && m_cursor + 2 < m_end && isDigit(m_cursor[2]))) {
                readNumber();
                continue;
            }
        }

        // ── Identifiers ──
        if (cls & CC_ALPHA) {
            readIdentifier();
            continue;
        }

        // ── String literals ──
        if (cls & CC_QUOTE) {
            readString();
            continue;
        }

        // ── Standalone dot (not part of a number) ──
        if (cls & CC_DOT) {
            m_tokens.push_back({MdlTokenType::Dot, {m_cursor, 1}, m_line, col});
            ++m_cursor;
            continue;
        }

        // ── Unknown character → error token ──
        m_tokens.push_back({MdlTokenType::Error, {m_cursor, 1}, m_line, col});
        ++m_cursor;
    }

    const u32 eofCol = static_cast<u32>(m_cursor - m_lineStart) + 1;
    m_tokens.push_back({MdlTokenType::EndOfFile, {}, m_line, eofCol});
}

void MdlTokenizer::skipWhitespaceAndComments() {
    for (;;) {
        // Skip whitespace
        while (m_cursor < m_end) {
            const u8 cls = classify(*m_cursor);
            if (cls & CC_SPACE) {
                ++m_cursor;
            } else if (cls & CC_NEWLINE) {
                ++m_cursor;
                ++m_line;
                m_lineStart = m_cursor;
            } else {
                break;
            }
        }

        // Skip // line comments
        if (m_cursor + 1 < m_end && m_cursor[0] == '/' && m_cursor[1] == '/') {
            m_cursor += 2;
            while (m_cursor < m_end && *m_cursor != '\n')
                ++m_cursor;
            // Don't consume the newline here; the outer loop will handle it
            // and count the line.
            continue;
        }

        break;
    }
}

void MdlTokenizer::readNumber() {
    const char* start = m_cursor;

    // Optional sign
    if (m_cursor < m_end && (*m_cursor == '+' || *m_cursor == '-'))
        ++m_cursor;

    // Integer part
    while (m_cursor < m_end && isDigit(*m_cursor))
        ++m_cursor;

    // Fractional part
    if (m_cursor < m_end && *m_cursor == '.') {
        ++m_cursor;
        while (m_cursor < m_end && isDigit(*m_cursor))
            ++m_cursor;
    }

    // Exponent part (e.g. 1e-3, 2.5E+10)
    if (m_cursor < m_end && (*m_cursor == 'e' || *m_cursor == 'E')) {
        ++m_cursor;
        if (m_cursor < m_end && (*m_cursor == '+' || *m_cursor == '-'))
            ++m_cursor;
        while (m_cursor < m_end && isDigit(*m_cursor))
            ++m_cursor;
    }

    const auto len = static_cast<std::size_t>(m_cursor - start);
    const u32 col = static_cast<u32>(start - m_lineStart) + 1;
    m_tokens.push_back({MdlTokenType::Number, {start, len}, m_line, col});
}

void MdlTokenizer::readIdentifier() {
    const char* start = m_cursor;
    ++m_cursor; // first char already verified as alpha/_
    while (m_cursor < m_end && isAlphaNum(*m_cursor))
        ++m_cursor;
    const auto len = static_cast<std::size_t>(m_cursor - start);
    const u32 col = static_cast<u32>(start - m_lineStart) + 1;
    m_tokens.push_back({MdlTokenType::Identifier, {start, len}, m_line, col});
}

void MdlTokenizer::readString() {
    ++m_cursor; // skip opening quote
    const char* start = m_cursor;

    while (m_cursor < m_end && *m_cursor != '"' && *m_cursor != '\n')
        ++m_cursor;

    const auto len = static_cast<std::size_t>(m_cursor - start);

    // Column points to the opening quote (start - 1)
    const u32 col = static_cast<u32>(start - 1 - m_lineStart) + 1;

    if (m_cursor < m_end && *m_cursor == '"') {
        m_tokens.push_back({MdlTokenType::String, {start, len}, m_line, col});
        ++m_cursor; // skip closing quote
    } else {
        // Unterminated string
        m_tokens.push_back({MdlTokenType::Error, {start, len}, m_line, col});
    }
}

} // namespace mdx
} // namespace whiteout
