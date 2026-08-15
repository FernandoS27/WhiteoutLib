// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Corpus test for D3 Animation (.ani) files, through the generic SNO reader.
///
/// This is deliberately the *reflection* path, not the hand-written native
/// parser in d3_native_test.cpp. Running the same structural claims through
/// both proves the registered type table describes the format, rather than
/// only the bespoke reader agreeing with itself.
///
/// It validates ANI_FILE_FORMAT_SPECIFICATION.md as re-derived in correction
/// pass 3: an .ani is a 56-byte `Anim` header carrying an array of 408-byte
/// `AnimPermutation` records, each of which owns its own bone names, curves,
/// attachments and root motion.
///
/// The previous revision of this file asserted a 448-byte flat `Anim` with
/// top-level `sdBoneNames` / `sdTranslDescs` / `sdRotDescs` / `sdScaleDescs`.
/// That model was wrong: it conflated the header with the first permutation,
/// which is why a multi-permutation clip appeared to have no second animation.
/// Those five field names no longer exist in the registry.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/sno_reader.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using whiteout::f32;
using whiteout::i32;
using whiteout::i64;
using whiteout::u32;
using whiteout::u8;
using whiteout::sno::SnoGroup;
using whiteout::sno::SnoReader;
using whiteout::sno::SnoValue;

namespace {

std::vector<u8> readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    std::vector<u8> buf(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

fs::path findCorpus() {
    for (const char* rel : {"Corpus/D3/Anim", "../Corpus/D3/Anim", "../../Corpus/D3/Anim",
                            "../../../Corpus/D3/Anim"}) {
        if (fs::is_directory(rel)) return rel;
    }
    return {};
}

/// Array size, or -1 when the field is missing or not an array. Keeping the
/// "missing" case distinct from "empty" is what caught the stale field names.
i64 arraySize(const SnoValue& obj, const char* name) {
    const auto* f = obj.field(name);
    if (!f || !f->isArray()) return -1;
    return static_cast<i64>(f->size());
}

i64 intField(const SnoValue& obj, const char* name) {
    const auto* f = obj.field(name);
    if (!f) return -1;
    if (f->isInt()) return f->asInt();
    if (f->isUint()) return static_cast<i64>(f->asUint());
    return -1;
}

} // namespace

TEST_CASE("D3 .ani corpus via the generic reader", "[d3][ani][corpus]") {
    const fs::path dir = findCorpus();
    if (dir.empty()) SKIP("D3 Anim corpus not found");

    SnoReader reader;
    size_t files = 0, parsed = 0, perms = 0, multiPerm = 0, named = 0;
    size_t withAppearance = 0, attachments = 0, quatKeys = 0;
    // every counter below must finish at zero
    size_t badMagic = 0, badVersion = 0, permCountBad = 0, missingPerms = 0;
    size_t boneNamesBad = 0, curvesBad = 0, attachBad = 0;
    size_t rootABad = 0, rootBBad = 0, inPlaceBad = 0, quatBad = 0;
    size_t inPlacePopulated = 0;
    size_t densityBad = 0, densityChecked = 0;
    f32 worstQuat = 0.0f;

    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".ani") continue;
        ++files;
        auto data = readFile(e.path());
        if (data.size() < 72) { ++badMagic; continue; }
        u32 magic = 0, version = 0;
        std::memcpy(&magic, data.data(), 4);
        std::memcpy(&version, data.data() + 4, 4);
        if (magic != 0xDEADBEEF) { ++badMagic; continue; }
        if ((version & 0xFFFFu) != 118) ++badVersion;

        auto file = reader.parse(data, SnoGroup::Animation);
        if (!file) continue;
        ++parsed;
        const auto& root = file->root;

        if (root.field("snoAppearance")) ++withAppearance;

        const i64 declared = intField(root, "dwPermutationCount");
        const i64 got = arraySize(root, "arPermutations");
        if (got < 0) { ++missingPerms; continue; }
        if (declared != got) ++permCountBad;

        const auto* parr = root.field("arPermutations");
        for (size_t i = 0; i < parr->size(); ++i) {
            const auto* pm = parr->at(i);
            if (!pm || !pm->isObject()) { ++missingPerms; continue; }
            ++perms;
            if (i > 0) ++multiPerm;

            const auto* nm = pm->field("szName");
            if (nm && nm->isString() && !nm->asString().empty()) ++named;

            const i64 bones = intField(*pm, "dwBoneCount");
            const i64 frames = intField(*pm, "dwFrameCount");

            // one bone name and one curve per channel per bone
            if (arraySize(*pm, "arBoneNames") != bones) ++boneNamesBad;
            for (const char* ch : {"arTranslationCurves", "arRotationCurves", "arScaleCurves"}) {
                if (arraySize(*pm, ch) != bones) ++curvesBad;
            }

            const i64 na = arraySize(*pm, "arAttachments");
            if (na != intField(*pm, "dwAttachmentCount")) ++attachBad;
            if (na > 0) attachments += static_cast<size_t>(na);

            // both per-frame tracks hold exactly dwFrameCount entries
            const i64 ra = arraySize(*pm, "arRootMotion");
            const i64 rb = arraySize(*pm, "arRootMotionInPlace");
            if (ra != frames) ++rootABad;
            if (rb != ra) ++rootBBad;

            // the in-place track is the total track with the net velocity
            // removed: B[k] == A[k] - A[last]*k/(frames-1)
            // An array of DT_VECTOR3D is held as a TYPED SnoArray (a
            // std::vector<SnoVec3>), not as a list of per-element SnoValues, so
            // it has to be read through asArray().asVec3Data() -- at(i) does not
            // apply to typed arrays.
            if (ra == rb && ra > 1) {
                const auto& av = pm->field("arRootMotion")->asArray();
                const auto& bv = pm->field("arRootMotionInPlace")->asArray();
                if (av.isVec3() && bv.isVec3()) {
                    const auto& A = av.asVec3Data();
                    const auto& B = bv.asVec3Data();
                    const auto last = A.back();
                    bool populated = false, ok = true;
                    const auto n = A.size();
                    for (size_t k = 0; k < n && k < B.size(); ++k) {
                        if (B[k].x != 0.0f || B[k].y != 0.0f || B[k].z != 0.0f) populated = true;
                        const f32 t = static_cast<f32>(k) / static_cast<f32>(n - 1);
                        if (std::abs((A[k].x - last.x * t) - B[k].x) > 1e-3f ||
                            std::abs((A[k].y - last.y * t) - B[k].y) > 1e-3f ||
                            std::abs((A[k].z - last.z * t) - B[k].z) > 1e-3f)
                            ok = false;
                    }
                    // an all-zero pair satisfies the identity trivially, so only
                    // the populated tracks actually test it
                    if (populated) {
                        ++inPlacePopulated;
                        if (!ok) ++inPlaceBad;
                    }
                }
            }

            // +0x50/54/58 are baked keyframe density, one per curve channel:
            // (float)(frames * bones) / (float)totalKeys, in curve-array order.
            // Bit-exact in f32 corpus-wide, so compare exactly (see spec 4.1).
            {
                const char* chan[3] = {"arTranslationCurves", "arRotationCurves",
                                       "arScaleCurves"};
                const char* dens[3] = {"flFramesPerTranslationKey", "flFramesPerRotationKey",
                                       "flFramesPerScaleKey"};
                for (int c = 0; c < 3; ++c) {
                    const auto* curves = pm->field(chan[c]);
                    const auto* stored = pm->field(dens[c]);
                    if (!curves || !curves->isArray() || !stored || !stored->isFloat()) continue;
                    i64 keys = 0;
                    for (size_t i = 0; i < curves->size(); ++i) {
                        const auto* curve = curves->at(i);
                        if (curve && curve->isObject()) keys += arraySize(*curve, "arKeys");
                    }
                    if (keys <= 0) continue;
                    const f32 pred = static_cast<f32>(frames * bones) / static_cast<f32>(keys);
                    ++densityChecked;
                    if (stored->asFloat() != pred) ++densityBad;
                }
            }

            // rotation keys are four SIGNED i16 over 32767
            const auto* rc = pm->field("arRotationCurves");
            if (rc && rc->isArray()) {
                for (size_t c = 0; c < rc->size(); ++c) {
                    const auto* curve = rc->at(c);
                    if (!curve || !curve->isObject()) continue;
                    if (arraySize(*curve, "arKeys") != intField(*curve, "dwKeyCount")) ++curvesBad;
                    const auto* keys = curve->field("arKeys");
                    if (!keys || !keys->isArray()) continue;
                    for (size_t k = 0; k < keys->size(); ++k) {
                        const auto* key = keys->at(k);
                        if (!key || !key->isObject()) continue;
                        const auto* q = key->field("tRotation");
                        if (!q || !q->isObject()) continue;
                        f32 comp[4] = {0, 0, 0, 0};
                        const char* cn[4] = {"nX", "nY", "nZ", "nW"};
                        bool have = true;
                        for (int t = 0; t < 4; ++t) {
                            const auto* w = q->field(cn[t]);
                            if (!w || !w->isWord()) { have = false; break; }
                            comp[t] = static_cast<f32>(static_cast<int16_t>(w->asWord())) / 32767.0f;
                        }
                        if (!have) continue;
                        ++quatKeys;
                        const f32 len = std::sqrt(comp[0] * comp[0] + comp[1] * comp[1] +
                                                  comp[2] * comp[2] + comp[3] * comp[3]);
                        worstQuat = std::max(worstQuat, std::abs(len - 1.0f));
                        if (std::abs(len - 1.0f) >= 0.01f) ++quatBad;
                    }
                }
            }
        }
    }

    std::cout << "\n=== D3 .ani corpus (generic reader) ===\n"
              << "files:              " << files << "   parsed: " << parsed << "\n"
              << "permutations:       " << perms << "   (extra beyond the first: "
              << multiPerm << ")\n"
              << "named:              " << named << "\n"
              << "appearance refs:    " << withAppearance << "\n"
              << "attachments:        " << attachments << "\n"
              << "rotation keys:      " << quatKeys
              << "   worst |q|-1: " << worstQuat << "\n"
              << "in-place tracks with data: " << inPlacePopulated << "\n"
              << "-- all must be 0 --\n"
              << "bad magic:          " << badMagic << "\n"
              << "unexpected version: " << badVersion << "\n"
              << "missing perm array: " << missingPerms << "\n"
              << "perm count:         " << permCountBad << "\n"
              << "bone names:         " << boneNamesBad << "\n"
              << "curves / key counts:" << curvesBad << "\n"
              << "attachment count:   " << attachBad << "\n"
              << "root motion A:      " << rootABad << "\n"
              << "root motion B:      " << rootBBad << "\n"
              << "in-place identity:  " << inPlaceBad << "\n"
              << "non-unit quats:     " << quatBad << "\n"
              << "key density:        " << densityBad << " of " << densityChecked << "\n";

    CHECK(files > 0);
    CHECK(parsed == files);
    CHECK(perms >= files);
    CHECK(multiPerm > 0);       // multi-permutation clips must actually appear
    CHECK(named == perms);
    CHECK(quatKeys > 0);
    CHECK(inPlacePopulated > 0);
    // The in-place track is the total track minus the constant-velocity ramp.
    // It is bit-exact on 32 of the 35 populated tracks; three are hand-authored
    // exceptions (SandShark_idle_intro_end_01, SoulRipper_attack_04,
    // SandShark_idle_intro_01 -- see ANI spec §9.1), so this is the exporter's
    // rule rather than a hard invariant. Assert the rule still explains the
    // overwhelming majority, and that no NEW exception appears.
    CHECK(inPlaceBad <= 3);
    CHECK(inPlacePopulated - inPlaceBad >= 32);

    CHECK(badMagic == 0);
    CHECK(badVersion == 0);
    CHECK(missingPerms == 0);
    CHECK(permCountBad == 0);
    CHECK(boneNamesBad == 0);
    CHECK(curvesBad == 0);
    CHECK(attachBad == 0);
    CHECK(rootABad == 0);
    CHECK(rootBBad == 0);
    CHECK(quatBad == 0);
    // +0x50/54/58 == (float)(frames*bones)/(float)totalKeys, one per channel.
    CHECK(densityChecked > 0);
    CHECK(densityBad == 0);
}
