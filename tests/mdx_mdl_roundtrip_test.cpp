// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MDX → MDL → MDL round-trip test suite.
//
// Goal: prove that the MDL text writer and the MDL text parser agree, i.e.
// that nothing is silently lost when a model is serialized to MDL and read
// back. The corpus at Corpus/MDL is mostly binary .mdx, so each file feeds
// the following pipeline:
//
//     .mdx ──Parser──▶ M0 ──writeModelToMdl──▶ T1 ──convertMdlToModel──▶ M1
//                                                │
//                              writeModelToMdl ──┘──▶ T2
//
// Three checks are performed, in both the WarcraftIII and the HiveWorkshop
// MDL dialects:
//
//   1. Binary check       — M0 and M1 are each serialized back to binary MDX
//      and the two byte buffers must be identical. The MDX writer is
//      deterministic, so this is a full structural equality test: any field
//      the MDL round-trip dropped or mangled changes the bytes. The first
//      differing offset is attributed to its containing MDX chunk.
//
//   2. Fixed-point check  — T1 must equal T2. The writer is deterministic,
//      so if the parser reproduced M1 faithfully, re-serializing yields the
//      exact same text. The *first differing line* between T1 and T2 points
//      straight at whichever field the MDL parser dropped or mangled.
//
//   3. Structural check   — M0 and M1 must contain the same number of
//      geosets, bones, materials, … . A fast, readable summary of which
//      whole sections failed to survive the round-trip.

#include <catch2/catch_all.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/mdx/types.h>
#include <whiteout/models/mdx/writer.h>

#include "whiteout/models/mdx/mdl_converter.h"
#include "whiteout/models/mdx/mdl_writer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::mdx;

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

static std::string readFileBytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Path -> UTF-8 std::string. The corpus contains non-ASCII filenames (Chinese,
// bracketed tags, …); fs::path::string() throws on the active code page, so
// always route display strings through u8string(), which never throws.
static std::string pathStr(const fs::path& p) {
    std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// Locate the Corpus/MDL directory regardless of where the test is run from.
static fs::path findCorpusDir() {
    for (auto candidate : {"Corpus/MDL", "../Corpus/MDL", "../../Corpus/MDL",
                           "C:/Projects/WhiteoutLib/Corpus/MDL"}) {
        if (fs::is_directory(candidate)) return fs::path(candidate);
    }
    return {};
}

// Collect every .mdx / .mdl file under the corpus directory, sorted.
static std::vector<fs::path> collectModels(const fs::path& corpusDir,
                                           const std::string& ext) {
    std::vector<fs::path> files;
    if (corpusDir.empty()) return files;
    for (const auto& entry : fs::recursive_directory_iterator(corpusDir)) {
        if (!entry.is_regular_file()) continue;
        auto e = pathStr(entry.path().extension());
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        if (e == ext) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

static const char* dialectName(MdlFormat fmt) {
    return fmt == MdlFormat::WarcraftIII ? "WarcraftIII" : "HiveWorkshop";
}

// Render a human-readable description of the first line on which two MDL
// documents diverge, with a few lines of surrounding context from each.
static std::string firstDiff(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<std::string> lines;
        std::string cur;
        for (char c : s) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') { cur.push_back(c); }
        }
        if (!cur.empty()) lines.push_back(cur);
        return lines;
    };
    auto la = split(a);
    auto lb = split(b);
    std::size_t n = std::min(la.size(), lb.size());
    std::size_t i = 0;
    while (i < n && la[i] == lb[i]) ++i;

    std::ostringstream os;
    if (i == n && la.size() == lb.size()) {
        os << "(identical)";
        return os.str();
    }
    os << "first divergence at line " << (i + 1) << " of "
       << la.size() << "/" << lb.size() << " lines\n";
    std::size_t ctxStart = i > 3 ? i - 3 : 0;
    for (std::size_t j = ctxStart; j < i; ++j)
        os << "      " << (j < la.size() ? la[j] : "") << "\n";
    os << "  T1> " << (i < la.size() ? la[i] : "<end of T1>") << "\n";
    os << "  T2> " << (i < lb.size() ? lb[i] : "<end of T2>") << "\n";
    return os.str();
}

// Per-structure counts of a Model — enough to spot whole sections that fail
// to survive a round-trip.
struct Counts {
    std::size_t sequences = 0, globalSequences = 0, textures = 0, materials = 0;
    std::size_t textureAnimations = 0, geosets = 0, geosetAnimations = 0;
    std::size_t bones = 0, helpers = 0, attachments = 0, pivotPoints = 0;
    std::size_t lights = 0, particleEmitters = 0, particleEmitters2 = 0;
    std::size_t ribbonEmitters = 0, cornEmitters = 0, eventObjects = 0;
    std::size_t cameras = 0, collisionShapes = 0, faceEffects = 0, bindPoses = 0;

    static Counts of(const Model& m) {
        Counts c;
        c.sequences = m.sequences.size();
        c.globalSequences = m.globalSequences.size();
        c.textures = m.textures.size();
        c.materials = m.materials.size();
        c.textureAnimations = m.textureAnimations.size();
        c.geosets = m.geosets.size();
        c.geosetAnimations = m.geosetAnimations.size();
        c.bones = m.bones.size();
        c.helpers = m.helpers.size();
        c.attachments = m.attachments.size();
        c.pivotPoints = m.pivotPoints.size();
        c.lights = m.lights.size();
        c.particleEmitters = m.particleEmitters.size();
        c.particleEmitters2 = m.particleEmitters2.size();
        c.ribbonEmitters = m.ribbonEmitters.size();
        c.cornEmitters = m.cornEmitters.size();
        c.eventObjects = m.eventObjects.size();
        c.cameras = m.cameras.size();
        c.collisionShapes = m.collisionShapes.size();
        c.faceEffects = m.faceEffects.size();
        c.bindPoses = m.bindPoses.size();
        return c;
    }
};

// Describe every field on which two Counts disagree.
static std::string countsDiff(const Counts& a, const Counts& b) {
    std::ostringstream os;
    auto cmp = [&](const char* name, std::size_t x, std::size_t y) {
        if (x != y) os << "    " << name << ": " << x << " -> " << y << "\n";
    };
    cmp("sequences", a.sequences, b.sequences);
    cmp("globalSequences", a.globalSequences, b.globalSequences);
    cmp("textures", a.textures, b.textures);
    cmp("materials", a.materials, b.materials);
    cmp("textureAnimations", a.textureAnimations, b.textureAnimations);
    cmp("geosets", a.geosets, b.geosets);
    cmp("geosetAnimations", a.geosetAnimations, b.geosetAnimations);
    cmp("bones", a.bones, b.bones);
    cmp("helpers", a.helpers, b.helpers);
    cmp("attachments", a.attachments, b.attachments);
    cmp("pivotPoints", a.pivotPoints, b.pivotPoints);
    cmp("lights", a.lights, b.lights);
    cmp("particleEmitters", a.particleEmitters, b.particleEmitters);
    cmp("particleEmitters2", a.particleEmitters2, b.particleEmitters2);
    cmp("ribbonEmitters", a.ribbonEmitters, b.ribbonEmitters);
    cmp("cornEmitters", a.cornEmitters, b.cornEmitters);
    cmp("eventObjects", a.eventObjects, b.eventObjects);
    cmp("cameras", a.cameras, b.cameras);
    cmp("collisionShapes", a.collisionShapes, b.collisionShapes);
    cmp("faceEffects", a.faceEffects, b.faceEffects);
    cmp("bindPoses", a.bindPoses, b.bindPoses);
    return os.str();
}

// Name the top-level MDX chunk that contains the given byte offset. MDX is a
// flat list of [tag(4)][size(4)][data] chunks following the "MDLX" magic, so a
// failing byte offset can be attributed to a concrete chunk (GEOS, BONE, ...).
static std::string chunkAtOffset(const std::vector<u8>& mdx, std::size_t off) {
    if (mdx.size() < 4) return "<too small>";
    std::size_t pos = 4; // skip "MDLX"
    while (pos + 8 <= mdx.size()) {
        char tag[5] = {char(mdx[pos]), char(mdx[pos + 1]), char(mdx[pos + 2]),
                       char(mdx[pos + 3]), 0};
        u32 size;
        std::memcpy(&size, &mdx[pos + 4], 4);
        std::size_t end = pos + 8 + size;
        if (off >= pos && off < end) {
            std::ostringstream os;
            os << tag << " chunk (" << "+" << (off - pos) << " of "
               << (size + 8) << " bytes)";
            return os.str();
        }
        if (end <= pos) break; // malformed — avoid infinite loop
        pos = end;
    }
    return "<past last chunk / header>";
}

// Describe the first byte on which two MDX buffers diverge.
static std::string binaryDiff(const std::vector<u8>& a, const std::vector<u8>& b) {
    std::ostringstream os;
    std::size_t n = std::min(a.size(), b.size());
    std::size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    if (i == n && a.size() == b.size()) { os << "(identical)"; return os.str(); }
    os << "MDX size " << a.size() << " (m0) vs " << b.size() << " (m1); "
       << "first differing byte at offset " << i << "\n"
       << "      in m0: " << chunkAtOffset(a, i) << "\n"
       << "      in m1: " << chunkAtOffset(b, i) << "\n";
    auto hexWindow = [&](const std::vector<u8>& buf, const char* label) {
        os << "    " << label << ":";
        for (std::size_t j = (i > 8 ? i - 8 : 0);
             j < i + 8 && j < buf.size(); ++j) {
            char h[8];
            std::snprintf(h, sizeof(h), j == i ? "[%02X]" : " %02X", buf[j]);
            os << h;
        }
        os << "\n";
    };
    hexWindow(a, "m0");
    hexWindow(b, "m1");
    return os.str();
}

// ============================================================================
// Round-trip driver
// ============================================================================

struct RoundtripResult {
    bool binaryOk = false;     // strongest: m0 and m1 serialize to identical MDX
    bool fixedPointOk = false; // writer/parser text fixed-point
    bool structuralOk = false; // matching per-section counts
    std::string detail;        // populated only on failure
};

// Round-trip one already-parsed model through MDL text in one dialect, then
// prove the result is byte-identical to the original by serializing both the
// original and the round-tripped model back to binary MDX and comparing.
//
//   m0 ──writeModelToMdl──▶ t1 ──convertMdlToModel──▶ m1
//   m0 ──Writer(MDX)──▶ mdx0        m1 ──Writer(MDX)──▶ mdx1   (must be equal)
static RoundtripResult roundtrip(const Model& m0, MdlFormat fmt) {
    RoundtripResult r;

    std::string t1 = writeModelToMdl(m0, fmt);

    std::vector<std::string> issues;
    Model m1 = convertMdlToModel(t1, issues);

    std::string t2 = writeModelToMdl(m1, fmt);
    r.fixedPointOk = (t1 == t2);

    Counts c0 = Counts::of(m0), c1 = Counts::of(m1);
    r.structuralOk = std::memcmp(&c0, &c1, sizeof(Counts)) == 0;

    // Full comparison: both models serialized to binary MDX must be identical.
    //
    // The HiveWorkshop dialect intentionally omits engine-only constructs that
    // its community tooling does not recognise (see MDL spec Appendix A):
    // the material Unfogged / SortPrimsNearZ flags and the WrapWidth /
    // WrapHeight / Unlit layer-shading flags. A HiveWorkshop round-trip can
    // therefore never reproduce those flag bits. Strip them from the reference
    // model so the binary check still verifies everything the dialect *is*
    // contracted to preserve, without flagging the documented omissions.
    Model ref0 = m0;
    if (fmt == MdlFormat::Hiveworkshop) {
        for (auto& mat : ref0.materials) {
            mat.flags = mat.flags &
                        ~(Material::Flag::Unfogged | Material::Flag::SortPrimsNearZ);
            for (auto& layer : mat.layers) {
                layer.shadingFlags = layer.shadingFlags &
                                     ~(Layer::ShadingFlag::WrapWidth |
                                       Layer::ShadingFlag::WrapHeight |
                                       Layer::ShadingFlag::Unlit);
            }
        }
    }

    Writer w0, w1;
    std::vector<u8> mdx0 = w0.write(ref0, MDLXFormat::MDX);
    std::vector<u8> mdx1 = w1.write(m1, MDLXFormat::MDX);
    r.binaryOk = (mdx0 == mdx1);

    if (!r.binaryOk || !r.fixedPointOk || !r.structuralOk) {
        std::ostringstream os;
        if (!r.binaryOk)
            os << "  binary mismatch (mdx of m0 vs mdx of m1):\n  "
               << binaryDiff(mdx0, mdx1);
        if (!r.structuralOk)
            os << "  structural mismatch (mdx model -> mdl model):\n"
               << countsDiff(c0, c1);
        if (!r.fixedPointOk)
            os << "  fixed-point mismatch (write -> parse -> write):\n  "
               << firstDiff(t1, t2);
        if (!issues.empty()) {
            os << "  parser issues (" << issues.size() << "):\n";
            for (std::size_t k = 0; k < issues.size() && k < 8; ++k)
                os << "    - " << issues[k] << "\n";
        }
        r.detail = os.str();
    }
    return r;
}

// Drive the whole corpus through one dialect and assert the round-trip holds.
static void runCorpus(MdlFormat fmt) {
    fs::path corpus = findCorpusDir();
    if (corpus.empty()) SKIP("Corpus/MDL directory not found");

    // Only game-authored assets are guaranteed to round-trip byte-exactly.
    // Third-party community models are produced by external tools and use
    // non-canonical MDX encodings (padding, ordering, optional-field quirks)
    // that the engine-faithful writer deliberately does not reproduce.
    // Restrict the sweep to the game-authored war3.w3mod subtree;
    // WHITEOUT_RT_SUBDIR overrides the scope (e.g. "." for the whole corpus).
    if (const char* sub = std::getenv("WHITEOUT_RT_SUBDIR")) {
        corpus /= sub;
    } else {
        corpus /= "war3.w3mod";
    }
    if (!fs::is_directory(corpus)) SKIP("corpus subdir not found: " << pathStr(corpus));

    std::vector<fs::path> files = collectModels(corpus, ".mdx");
    if (files.empty()) SKIP("No .mdx files found under Corpus/MDL");

    // Optional cap for fast iteration: WHITEOUT_RT_LIMIT=N processes only the
    // first N files. Unset -> whole corpus.
    if (const char* lim = std::getenv("WHITEOUT_RT_LIMIT")) {
        std::size_t n = std::strtoul(lim, nullptr, 10);
        if (n > 0 && n < files.size()) files.resize(n);
    }

    std::size_t parsed = 0, parseFail = 0;
    std::size_t binaryFail = 0, fixedPointFail = 0, structuralFail = 0;
    std::vector<std::string> failReports;

    std::size_t idx = 0;
    for (const auto& path : files) {
        std::string rel = pathStr(fs::relative(path, corpus));
        if (++idx % 100 == 0)
            std::cout << "  [" << idx << "/" << files.size() << "] binary fails: "
                      << binaryFail << "  (at " << rel << ")" << std::endl;
        Model m0;
        try {
            std::string bytes = readFileBytes(path);
            if (bytes.empty()) { ++parseFail; continue; }
            Parser parser;
            m0 = parser.parse(
                std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()),
                                    bytes.size()),
                MDLXFormat::MDX);
            ++parsed;
        } catch (const std::exception& e) {
            ++parseFail;
            failReports.push_back(rel + ": MDX parse threw: " + e.what());
            continue;
        }

        RoundtripResult r;
        try {
            r = roundtrip(m0, fmt);
        } catch (const std::exception& e) {
            ++binaryFail;
            failReports.push_back(rel + ": round-trip threw: " + e.what());
            continue;
        }

        if (!r.binaryOk) ++binaryFail;
        if (!r.fixedPointOk) ++fixedPointFail;
        if (!r.structuralOk) ++structuralFail;
        if (!r.detail.empty() && failReports.size() < 2000)
            failReports.push_back(rel + ":\n" + r.detail);
    }

    std::cout << "\n=== MDX -> MDL round-trip (" << dialectName(fmt)
              << " dialect) ===\n"
              << "  files:               " << files.size() << "\n"
              << "  parsed:              " << parsed << "\n"
              << "  MDX parse failures:  " << parseFail << "\n"
              << "  binary failures:     " << binaryFail << "\n"
              << "  fixed-point failures:" << fixedPointFail << "\n"
              << "  structural failures: " << structuralFail << "\n";

    std::size_t const showLimit =
        std::getenv("WHITEOUT_RT_ALL") ? failReports.size() : std::size_t(25);
    std::size_t shown = 0;
    for (const auto& report : failReports) {
        if (shown++ >= showLimit) {
            std::cout << "  ... and " << (failReports.size() - showLimit)
                      << " more\n";
            break;
        }
        std::cout << "\n[" << dialectName(fmt) << "] " << report << "\n";
    }
    std::cout << std::flush;

    INFO("dialect: " << dialectName(fmt));
    CHECK(parseFail == 0);
    CHECK(structuralFail == 0);
    CHECK(fixedPointFail == 0);
    CHECK(binaryFail == 0);
}

// Diagnostic: round-trip a single named model and dump the full detail.
// Override the target with the WHITEOUT_RT_FILE environment variable.
TEST_CASE("mdx_mdl_roundtrip_diagnostic", "[mdl][roundtrip][diag]") {
    fs::path corpus = findCorpusDir();
    if (corpus.empty()) SKIP("Corpus/MDL directory not found");

    const char* env = std::getenv("WHITEOUT_RT_FILE");
    fs::path target = corpus / (env ? env : "Ace/Ace.mdx");
    std::string bytes = readFileBytes(target);
    if (bytes.empty()) SKIP("diagnostic file not found: " << pathStr(target));

    Parser parser;
    Model m0 = parser.parse(
        std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()),
                            bytes.size()),
        MDLXFormat::MDX);

    for (MdlFormat fmt : {MdlFormat::WarcraftIII, MdlFormat::Hiveworkshop}) {
        RoundtripResult r = roundtrip(m0, fmt);
        std::cout << "\n=== diagnostic: " << pathStr(target) << " ["
                  << dialectName(fmt) << "] ===\n"
                  << "  binaryOk=" << r.binaryOk
                  << " fixedPointOk=" << r.fixedPointOk
                  << " structuralOk=" << r.structuralOk << "\n"
                  << r.detail << std::flush;
        INFO("dialect " << dialectName(fmt) << "\n" << r.detail);
        CHECK(r.binaryOk);
    }
}

// ============================================================================
// Corpus round-trip tests
// ============================================================================

TEST_CASE("mdx_mdl_roundtrip_warcraft3", "[mdl][roundtrip][corpus]") {
    runCorpus(MdlFormat::WarcraftIII);
}

TEST_CASE("mdx_mdl_roundtrip_hiveworkshop", "[mdl][roundtrip][corpus]") {
    runCorpus(MdlFormat::Hiveworkshop);
}

// Round-trip the handful of native .mdl files in the corpus: parse the text,
// then verify writer/parser fixed-point in both dialects.
TEST_CASE("mdl_text_corpus_roundtrip", "[mdl][roundtrip][corpus]") {
    fs::path corpus = findCorpusDir();
    if (corpus.empty()) SKIP("Corpus/MDL directory not found");

    // Game-authored assets only — third-party models use non-canonical
    // encodings that do not round-trip byte-exactly (see runCorpus()).
    corpus /= "war3.w3mod";
    if (!fs::is_directory(corpus)) SKIP("war3.w3mod corpus subdir not found");

    std::vector<fs::path> files = collectModels(corpus, ".mdl");
    if (files.empty()) SKIP("No .mdl files found under Corpus/MDL");

    std::size_t fail = 0;
    for (const auto& path : files) {
        std::string rel = pathStr(fs::relative(path, corpus));
        std::string text = readFileBytes(path);
        std::vector<std::string> issues;
        Model m0 = convertMdlToModel(text, issues);

        for (MdlFormat fmt : {MdlFormat::WarcraftIII, MdlFormat::Hiveworkshop}) {
            RoundtripResult r = roundtrip(m0, fmt);
            if (!r.binaryOk || !r.fixedPointOk || !r.structuralOk) {
                ++fail;
                std::cout << "\n[" << dialectName(fmt) << "] " << rel << ":\n"
                          << r.detail << std::flush;
            }
        }
    }
    CHECK(fail == 0);
}
