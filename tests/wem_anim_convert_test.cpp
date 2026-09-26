// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G-G, the library's half (WEM_ANIMATION_RUNTIME_PLAN.md §5): `ConvertClips`
/// both ways on corpus documents. Every clip, converted, poses under its new
/// rule and storage within G-B's 0.01 of the original under its own; exported
/// to the new game it crosses nothing more; a second conversion is a no-op.

#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>
#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/anim/crossing.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/reflect_bytes.h>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

/// How far two palettes of @p clip part, @p before under @p fromStorage and
/// @p after under @p toStorage, over a few times, as a fraction of what G-B
/// allows: 0.01 in the rotation rows, and in the translation row 0.5% of the
/// model's height. The conversion holds each rotation to 0.1 degree, which a
/// bone 250 units out turns into 0.44 of translation on a Warcraft III rig, so
/// a fixed 0.01 there would ask for a thousandth of the design's tolerance.
f32 PaletteDelta(const Document& before, const Document& after, u32 clip, Game fromStorage,
                 Game toStorage) {
    const u32 model = before.clips[clip].model;
    const Animator old(before, model, fromStorage);
    const Animator now(after, model, toStorage);
    const f32 duration = before.clips[clip].duration;
    const f32 reach = 5.0f * TolerancesOf(before.models[model]).translation;
    f32 worst = 0;
    // Played as the clip plays, looping or not, and short of its end: at the
    // end itself a looping M3 track has wrapped to its first key, the same
    // instant of the loop as Warcraft III's last.
    for (const f32 at : {0.0f, 0.13f, 0.37f, 0.5f, 0.71f, 0.94f, 0.995f}) {
        Mix mix;
        mix.plays.push_back(Play{clip, at * duration, 1.0f, true});
        mix.globals = false;
        Pose a, b;
        old.evaluate(mix, a);
        now.evaluate(mix, b);
        for (std::size_t n = 0; n < a.skinning.size(); ++n) {
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    const f32 d = std::fabs(a.skinning[n].data[i][j] - b.skinning[n].data[i][j]);
                    worst = std::max(worst, d / (i == 3 ? std::max(reach, 0.01f) : 0.01f));
                }
            }
        }
    }
    return worst;
}

std::vector<u32> AllClips(const Document& document) {
    std::vector<u32> out(document.clips.size());
    for (u32 c = 0; c < out.size(); ++c) {
        out[c] = c;
    }
    return out;
}

/// Converts @p document to @p to and checks G-G on it.
void CheckConversion(const Document& source, ReadRule to, Game fromStorage, Game toStorage,
                     ProfileId target) {
    Document document = source;
    const std::vector<u32> clips = AllClips(document);
    Diagnostics diagnostics;
    const u32 converted = ConvertClips(document, clips, to, fromStorage, toStorage, diagnostics);
    CHECK(converted > 0);
    for (u32 c = 0; c < document.clips.size(); ++c) {
        INFO("clip " << c << " '" << document.clips[c].name << "'");
        CHECK(document.clips[c].readRule == to);
        CHECK(PaletteDelta(source, document, c, fromStorage, toStorage) < 1.0f);
    }
    // Exported to the new game, nothing crosses any more.
    Document staged = document;
    Diagnostics exported;
    CHECK(ResampleForTarget(staged, target, toStorage, exported) == 0);
    CHECK(exported.countOf(DiagCode::AnimTrackResampled) == 0);
    // And converting again changes nothing.
    Document again = document;
    Diagnostics twice;
    CHECK(ConvertClips(again, clips, to, toStorage, toStorage, twice) == 0);
    const bool same = ReflectBytes(again) == ReflectBytes(document);
    CHECK(same);
}

} // namespace

TEST_CASE("wem corpus .mdx clips convert to StarCraft II and play the same",
          "[wem][anim][convert][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    const std::size_t limit = test::sweepLimit(files.size(), 8);
    const MdxConverter converter;
    u32 documents = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        mdx::Parser parser;
        const mdx::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        const Result<Document> read = converter.fromMdx(source);
        if (!read.ok() || read->clips.empty()) {
            continue;
        }
        INFO(test::pathText(files[i]));
        CheckConversion(*read.value, ReadRule::Sc2, Game::Warcraft, Game::StarCraft, ProfileId::Sc2);
        ++documents;
    }
    CHECK(documents > 0);
}

TEST_CASE("wem corpus .m3 clips convert to Warcraft III and play the same",
          "[wem][anim][convert][corpus]") {
    const auto files =
        test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    const std::size_t limit = test::sweepLimit(files.size(), 8);
    const M3Converter converter;
    u32 documents = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        m3::Parser parser;
        const m3::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        const Result<Document> read = converter.fromM3(source, ProfileId::Sc2);
        if (!read.ok() || read->clips.empty()) {
            continue;
        }
        INFO(test::pathText(files[i]));
        CheckConversion(*read.value, ReadRule::Wc3, Game::StarCraft, Game::Warcraft,
                        ProfileId::Wc3Reforged);
        ++documents;
    }
    CHECK(documents > 0);
}
