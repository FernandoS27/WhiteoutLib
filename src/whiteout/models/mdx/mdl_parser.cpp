// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdl_parser.h"
#include "mdl_tokenizer.h"

#include <cstdlib>

namespace whiteout {
namespace mdx {

namespace {

// ─── Token stream helper ─────────────────────────────────────────────────────

class TokenStream {
public:
    explicit TokenStream(std::vector<MdlToken> tokens)
        : m_tokens(std::move(tokens)), m_pos(0) {}

    const MdlToken& peek() const { return m_tokens[m_pos]; }
    const MdlToken& peek(std::size_t ahead) const {
        auto idx = m_pos + ahead;
        return idx < m_tokens.size() ? m_tokens[idx] : m_tokens.back();
    }
    const MdlToken& advance() {
        if (m_pos + 1 < m_tokens.size()) return m_tokens[m_pos++];
        return m_tokens[m_pos]; // stay on EOF sentinel
    }
    bool atEnd() const { return m_tokens[m_pos].type == MdlTokenType::EndOfFile; }

    bool check(MdlTokenType type) const { return peek().type == type; }
    bool match(MdlTokenType type) {
        if (check(type)) { advance(); return true; }
        return false;
    }

    // Consume a token of the given type, or record an error and return false.
    bool expect(MdlTokenType type, std::vector<MdlParseError>& errors, const char* context) {
        if (check(type)) { advance(); return true; }
        auto& t = peek();
        errors.push_back({std::string("expected ") + context + " but got '" +
                              std::string(t.text) + "'",
                          t.line, t.column});
        return false;
    }

    std::size_t pos() const { return m_pos; }
    void setPos(std::size_t p) { m_pos = p; }

private:
    std::vector<MdlToken> m_tokens;
    std::size_t m_pos;
};

// ─── Numeric conversion ─────────────────────────────────────────────────────

static f64 toNumber(std::string_view text) {
    // std::from_chars for double is not universally available on all MSVC
    // versions for all locales, so we use strtod via a null-terminated copy.
    // The text is typically short (< 30 chars).
    char buf[64];
    auto len = text.size() < 63 ? text.size() : 63;
    std::memcpy(buf, text.data(), len);
    buf[len] = '\0';
    return std::strtod(buf, nullptr);
}

static i32 toInt(std::string_view text) {
    char buf[32];
    auto len = text.size() < 31 ? text.size() : 31;
    std::memcpy(buf, text.data(), len);
    buf[len] = '\0';
    return static_cast<i32>(std::strtol(buf, nullptr, 10));
}

// ─── Parser core ─────────────────────────────────────────────────────────────

class ParserImpl {
public:
    explicit ParserImpl(TokenStream& ts, std::vector<MdlParseError>& errors)
        : m_ts(ts), m_errors(errors) {}

    // Parse all top-level blocks.
    std::vector<MdlNode> parseDocument() {
        std::vector<MdlNode> roots;
        while (!m_ts.atEnd()) {
            if (m_ts.check(MdlTokenType::Identifier)) {
                roots.push_back(parseBlock());
            } else {
                // Skip unexpected tokens at top level.
                auto& t = m_ts.advance();
                m_errors.push_back({"unexpected token at top level: '" +
                                        std::string(t.text) + "'",
                                    t.line, t.column});
            }
        }
        return roots;
    }

private:
    TokenStream& m_ts;
    std::vector<MdlParseError>& m_errors;

    // ── Block parsing ────────────────────────────────────────────────────

    MdlNode parseBlock() {
        MdlNode node;
        node.name = std::string(m_ts.advance().text); // consume block name

        // Collect header parameters (strings and numbers) before '{'
        while (!m_ts.atEnd() && !m_ts.check(MdlTokenType::OpenBrace)) {
            if (m_ts.check(MdlTokenType::String)) {
                node.headerParams.push_back(MdlValue{std::string(m_ts.advance().text)});
            } else if (m_ts.check(MdlTokenType::Number)) {
                node.headerParams.push_back(MdlValue{toNumber(m_ts.advance().text)});
            } else {
                break; // unexpected token
            }
        }

        if (!m_ts.expect(MdlTokenType::OpenBrace, m_errors, "'{'")) {
            return node;
        }

        parseBlockBody(node);

        m_ts.expect(MdlTokenType::CloseBrace, m_errors, "'}'");
        return node;
    }

    void parseBlockBody(MdlNode& node) {
        while (!m_ts.atEnd() && !m_ts.check(MdlTokenType::CloseBrace)) {
            if (!m_ts.check(MdlTokenType::Identifier)) {
                // Could be a raw value line (e.g. in VertexGroup: "0,")
                // or a brace-delimited array element (e.g. in Vertices: "{ 1, 2, 3 },")
                if (m_ts.check(MdlTokenType::Number) || m_ts.check(MdlTokenType::OpenBrace)) {
                    // Treat as an anonymous property with values
                    node.children.push_back(parseAnonymousValues());
                    continue;
                }
                auto& t = m_ts.advance();
                m_errors.push_back({"unexpected token in block body: '" +
                                        std::string(t.text) + "'",
                                    t.line, t.column});
                continue;
            }

            // We have an identifier. Determine what follows:
            //  1. "static" keyword prefix  → property
            //  2. IDENT '{'                → sub-block (e.g. Layer, Anim, Target, Bitmap)
            //  3. IDENT STRING|NUMBER+ '{' → sub-block with header params
            //  4. IDENT NUMBER '{' IDENT ','  → animation track (count + interp)
            //  5. IDENT NUMBER '{'         → could be track (EventTrack) or block (Vertices N {)
            //  6. IDENT value ','          → property
            //  7. IDENT ','                → bare flag

            auto& first = m_ts.peek();

            // Handle "static" prefix
            if (first.text == "static") {
                m_ts.advance(); // consume "static"
                node.children.push_back(parseProperty(true));
                continue;
            }

            // Look ahead to classify
            auto& second = m_ts.peek(1);

            // IDENT ','  → bare flag
            if (second.type == MdlTokenType::Comma) {
                MdlProperty prop;
                prop.name = std::string(m_ts.advance().text);
                m_ts.advance(); // consume comma
                node.children.push_back(std::move(prop));
                continue;
            }

            // IDENT '{' → either sub-block or property with vector value
            // Disambiguate by peeking inside the brace:
            //   - First token is Number → property (e.g. Alpha {255, 200, 0},)
            //   - First token is Identifier → sub-block (e.g. Layer { FilterMode None, })
            //   - First token is OpenBrace → sub-block with vector data (e.g. Vertices { {1,2,3}, })
            //   - First token is CloseBrace → empty sub-block
            if (second.type == MdlTokenType::OpenBrace) {
                auto& insideBrace = m_ts.peek(2);
                if (insideBrace.type == MdlTokenType::Number ||
                    insideBrace.type == MdlTokenType::String) {
                    // Property with vector value: Alpha {255, 200, 0},
                    node.children.push_back(parseProperty(false));
                } else {
                    // Sub-block: Layer { ... }, SegmentColor { Color { ... } ... }
                    node.children.push_back(parseBlock());
                    m_ts.match(MdlTokenType::Comma); // trailing comma after block
                }
                continue;
            }

            // IDENT NUMBER ... → could be track, block, or property
            if (second.type == MdlTokenType::Number) {
                if (tryParseAsTrackOrBlock(node)) {
                    continue;
                }
                // Fall through to property
            }

            // IDENT STRING ... → block or property
            if (second.type == MdlTokenType::String) {
                auto& third = m_ts.peek(2);
                if (third.type == MdlTokenType::OpenBrace ||
                    third.type == MdlTokenType::Number) {
                    // sub-block with string param: Anim "Stand 1" { ... }
                    // or: Bone "name" { ... } — third could be number then '{'
                    node.children.push_back(parseBlock());
                    m_ts.match(MdlTokenType::Comma); // trailing comma after block
                    continue;
                }
                // Otherwise property: Image "foo.blp",
                node.children.push_back(parseProperty(false));
                continue;
            }

            // IDENT IDENT ... → property (e.g. FilterMode None, or GeosetId Multiple,)
            if (second.type == MdlTokenType::Identifier) {
                node.children.push_back(parseProperty(false));
                continue;
            }

            // Fallback: treat as property
            node.children.push_back(parseProperty(false));
        }
    }

    // Attempt to parse "IDENT NUMBER ..." as either an animation track or a
    // sub-block with numeric header params. Returns true if successfully parsed.
    bool tryParseAsTrackOrBlock(MdlNode& parent) {
        // Pattern: IDENT NUMBER '{'
        // Could be:
        //   - Animation track:  Translation 402 { Linear, ... }
        //   - Data block:       Vertices 2085 { ... }
        //   - Event track:      EventTrack 1 { 0, ... }
        //
        // Disambiguation: peek inside the brace to see if first child is
        // an interp keyword or a number followed by colon (keyframe).

        // Check that we have IDENT NUMBER+ '{'
        std::size_t lookahead = 1; // position 0 = IDENT
        while (m_ts.peek(lookahead).type == MdlTokenType::Number) {
            ++lookahead;
        }

        if (m_ts.peek(lookahead).type != MdlTokenType::OpenBrace) {
            // Not a block/track — must be a property: IDENT NUMBER ','
            parent.children.push_back(parseProperty(false));
            return true;
        }

        // Now peek past the '{' to see what's inside
        auto insideIdx = lookahead + 1;
        auto& insideToken = m_ts.peek(insideIdx);

        // Check for interpolation keyword → animation track
        bool isTrack = false;
        if (insideToken.type == MdlTokenType::Identifier) {
            auto txt = insideToken.text;
            if (txt == "Linear" || txt == "Hermite" || txt == "Bezier" ||
                txt == "DontInterp") {
                isTrack = true;
            }
        }

        // Check for bare number followed by colon → keyframes (EventTrack-like)
        // Or bare number followed by comma → EventTrack values
        if (!isTrack && insideToken.type == MdlTokenType::Number) {
            auto& afterNum = m_ts.peek(insideIdx + 1);
            if (afterNum.type == MdlTokenType::Colon || afterNum.type == MdlTokenType::Comma) {
                isTrack = true;
            }
        }

        if (isTrack) {
            parent.children.push_back(parseAnimTrack());
            m_ts.match(MdlTokenType::Comma); // trailing comma after track
            return true;
        }

        // It's a data block (e.g. Vertices 2085 { ... })
        parent.children.push_back(parseBlock());
        m_ts.match(MdlTokenType::Comma); // trailing comma after block
        return true;
    }

    // ── Property parsing ─────────────────────────────────────────────────

    MdlProperty parseProperty(bool isStatic) {
        MdlProperty prop;
        prop.isStatic = isStatic;
        prop.name = std::string(m_ts.advance().text); // consume name

        // Collect values until comma or end of block.
        // Break early if the next token is an identifier or open-brace,
        // which indicates we've left the property's value context and
        // reached the next entry in the parent block.
        while (!m_ts.atEnd() && !m_ts.check(MdlTokenType::Comma) &&
               !m_ts.check(MdlTokenType::CloseBrace)) {
            prop.values.push_back(parseValue());
            if (m_ts.check(MdlTokenType::Identifier) || m_ts.check(MdlTokenType::OpenBrace) ||
                m_ts.check(MdlTokenType::Number)) {
                break;
            }
        }

        m_ts.match(MdlTokenType::Comma); // consume optional trailing comma
        return prop;
    }

    // Parse anonymous values (numbers or vectors without a leading identifier).
    // Used inside data blocks like VertexGroup { 0, 0, ... } or Vertices { { 1,2,3 }, ... }
    MdlProperty parseAnonymousValues() {
        MdlProperty prop;
        prop.name = ""; // anonymous

        // Collect values until we hit something that starts a new entry
        prop.values.push_back(parseValue());
        m_ts.match(MdlTokenType::Comma);

        return prop;
    }

    // ── Value parsing ────────────────────────────────────────────────────

    MdlValue parseValue() {
        if (m_ts.check(MdlTokenType::Number)) {
            return MdlValue{toNumber(m_ts.advance().text)};
        }
        if (m_ts.check(MdlTokenType::String)) {
            return MdlValue{std::string(m_ts.advance().text)};
        }
        if (m_ts.check(MdlTokenType::Identifier)) {
            // Identifiers as values (e.g. "None", "Multiple", "Linear", filter modes)
            return MdlValue{std::string(m_ts.advance().text)};
        }
        if (m_ts.check(MdlTokenType::OpenBrace)) {
            return parseValueArray();
        }

        // Error recovery
        auto& t = m_ts.advance();
        m_errors.push_back({"unexpected token in value: '" + std::string(t.text) + "'",
                            t.line, t.column});
        return MdlValue{0.0};
    }

    // Parse { val, val, ... }
    MdlValue parseValueArray() {
        m_ts.advance(); // consume '{'
        MdlValue::Array arr;

        while (!m_ts.atEnd() && !m_ts.check(MdlTokenType::CloseBrace)) {
            arr.push_back(parseValue());
            m_ts.match(MdlTokenType::Comma); // optional comma between elements
        }

        m_ts.expect(MdlTokenType::CloseBrace, m_errors, "'}'");
        return MdlValue{std::move(arr)};
    }

    // ── Animation track parsing ──────────────────────────────────────────

    MdlAnimTrack parseAnimTrack() {
        MdlAnimTrack track;
        track.name = std::string(m_ts.advance().text); // consume track name

        // Parse count
        if (m_ts.check(MdlTokenType::Number)) {
            track.count = static_cast<u32>(toNumber(m_ts.advance().text));
        }

        m_ts.expect(MdlTokenType::OpenBrace, m_errors, "'{'");

        // Check for interpolation type keyword
        if (m_ts.check(MdlTokenType::Identifier)) {
            auto txt = m_ts.peek().text;
            if (txt == "Linear" || txt == "Hermite" || txt == "Bezier" ||
                txt == "DontInterp") {
                track.interpolation = std::string(m_ts.advance().text);
                m_ts.match(MdlTokenType::Comma);
            }
        }

        // Optional: GlobalSeqId N,
        if (m_ts.check(MdlTokenType::Identifier) && m_ts.peek().text == "GlobalSeqId") {
            m_ts.advance();
            if (m_ts.check(MdlTokenType::Number)) {
                track.globalSequenceId = static_cast<u32>(toNumber(m_ts.advance().text));
            }
            m_ts.match(MdlTokenType::Comma);
        }

        // Parse keyframes
        while (!m_ts.atEnd() && !m_ts.check(MdlTokenType::CloseBrace)) {
            if (m_ts.check(MdlTokenType::Number)) {
                auto& nextAfterNum = m_ts.peek(1);
                if (nextAfterNum.type == MdlTokenType::Colon) {
                    // Standard keyframe: TIME: value,
                    track.keyframes.push_back(parseKeyframe(
                        track.interpolation == "Hermite" || track.interpolation == "Bezier"));
                } else {
                    // EventTrack-style bare value: TIME,
                    MdlKeyframe kf;
                    kf.time = toInt(m_ts.advance().text);
                    kf.value = MdlValue{static_cast<f64>(kf.time)};
                    m_ts.match(MdlTokenType::Comma);
                    track.keyframes.push_back(std::move(kf));
                }
            } else {
                // Unexpected token in track body
                auto& t = m_ts.advance();
                m_errors.push_back({"unexpected token in animation track: '" +
                                        std::string(t.text) + "'",
                                    t.line, t.column});
            }
        }

        m_ts.expect(MdlTokenType::CloseBrace, m_errors, "'}'");
        return track;
    }

    MdlKeyframe parseKeyframe(bool expectTangents) {
        MdlKeyframe kf;
        kf.time = toInt(m_ts.advance().text);     // consume timestamp
        m_ts.expect(MdlTokenType::Colon, m_errors, "':'");
        kf.value = parseValue();
        m_ts.match(MdlTokenType::Comma);

        // Check for InTan / OutTan (Hermite/Bezier)
        if (expectTangents && m_ts.check(MdlTokenType::Identifier) &&
            m_ts.peek().text == "InTan") {
            m_ts.advance(); // consume "InTan"
            kf.inTan = parseValue();
            m_ts.match(MdlTokenType::Comma);

            if (m_ts.check(MdlTokenType::Identifier) && m_ts.peek().text == "OutTan") {
                m_ts.advance(); // consume "OutTan"
                kf.outTan = parseValue();
                m_ts.match(MdlTokenType::Comma);
            }
            kf.hasTangents = true;
        }

        return kf;
    }
};

} // anonymous namespace

// ─── Public API ──────────────────────────────────────────────────────────────

MdlDocument MdlParser::parse(std::string_view source) {
    auto tokens = MdlTokenizer::tokenize(source);
    TokenStream ts(std::move(tokens));

    MdlDocument doc;
    ParserImpl parser(ts, doc.errors);
    doc.roots = parser.parseDocument();
    return doc;
}

} // namespace mdx
} // namespace whiteout
