// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Verifies the generated native parsers against the generic reader.
///
/// The generic SnoValue reader is the trusted reference: it validates
/// 11347/11347 Appearance files and 15258/15258 Anim files against the format
/// specs.  The native parsers are a fast path over the same bytes, so for every
/// file they must agree field for field.  Any disagreement is a native bug.
///
/// Also measures the speed difference, which is the reason the native path
/// exists at all.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/d3/native/d3_native.h>
#include <whiteout/sno/d3/native/geometry.h>
#include <whiteout/sno/sno_reader.h>

// White-box.  A few checks below read the on-disk image directly to prove the
// parsed result agrees with the bytes it came from, so they need the packed
// layouts.  layout.h is build-internal and not installed -- this test is the
// only thing that reaches past the public headers, which is why this target
// alone carries src/ on its include path (tests/CMakeLists.txt).
#include "whiteout/sno/d3/native/layout.h"

#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::sno;
namespace nat = whiteout::sno::d3::native;

static std::vector<u8> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    std::vector<u8> buf(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

static fs::path findCorpus() {
    for (auto c : {"Corpus/D3", "../Corpus/D3", "../../Corpus/D3"})
        if (fs::is_directory(c)) return c;
    return {};
}

/// Pull an integer field out of the generic reader's tree.
static bool genericInt(const SnoValue& root, const char* name, i64& out) {
    auto* f = root.field(name);
    if (!f) return false;
    if (f->isInt())  { out = f->asInt();  return true; }
    if (f->isUint()) { out = static_cast<i64>(f->asUint()); return true; }
    return false;
}

TEST_CASE("D3 native: Appearance matches the generic reader", "[d3][native]") {
    auto base = findCorpus();
    if (base.empty()) SKIP("D3 corpus not found");
    const fs::path dir = base / "Appearances";
    if (!fs::is_directory(dir)) SKIP("Appearances corpus not found");

    SnoReader reader;
    size_t files = 0, nativeOk = 0, compared = 0, mismatch = 0, withBones = 0;
    size_t totalBones = 0, totalSubObjects = 0, totalMaterials = 0, totalLooks = 0;
    // Resolved-content counters: a parse that returns zero of these has read
    // the descriptors but not the data they point at.
    size_t namedBones = 0, namedMeshes = 0, namedMaterials = 0, namedLooks = 0;
    size_t vertices = 0, indices = 0, influences = 0, capsules = 0, hardpoints = 0;
    size_t octreeNodes = 0, octreeLeaves = 0, octreePrims = 0, filesWithOctree = 0;
    size_t variants = 0, variantLookMismatch = 0, tagEntries = 0, badTagType = 0;
    size_t texEntries = 0, shaderMapRefs = 0;

    for (const auto& e : fs::directory_iterator(dir)) {
        if (files >= 2000) break;
        if (!e.is_regular_file() || e.path().extension() != ".app") continue;
        ++files;
        auto data = readFile(e.path());
        if (data.size() < 32) continue;

        auto nv = nat::parseAppearances(data);
        if (!nv) continue;
        ++nativeOk;

        auto gen = reader.parse(data, SnoGroup::Appearance);
        if (!gen) continue;
        ++compared;

        // Scalar agreement.
        struct Check { const char* name; i64 nativeVal; };
        const Check checks[] = {
            {"dwBoneCount", nv->dwBoneCount},
            {"dwMaterialCount", nv->dwMaterialCount},
            {"dwLookCount", nv->dwLookCount},
            {"eObjectType", nv->eObjectType},
        };
        for (const auto& c : checks) {
            i64 g = 0;
            if (!genericInt(gen->root, c.name, g)) continue;
            if (g != c.nativeVal) {
                if (mismatch < 5)
                    std::cout << "  MISMATCH " << e.path().filename().string() << " " << c.name
                              << ": native=" << c.nativeVal << " generic=" << g << "\n";
                ++mismatch;
            }
        }

        // Array resolution: element counts must line up with the declared counts.
        const auto& bones = nv->arBones;
        totalBones += bones.size();
        totalLooks += nv->arLooks.size();
        totalMaterials += nv->arMaterials.size();
        capsules += nv->arCollisionCapsules.size();
        hardpoints += nv->arHardpoints.size();
        if (!bones.empty()) ++withBones;

        for (const auto& b : bones)
            if (!b.szName.empty()) ++namedBones;
        for (const auto& l : nv->arLooks)
            if (!l.szName.empty()) ++namedLooks;

        // The engine stores two GeoSets; the corpus only ever fills the first,
        // but both are walked so a regression in the second would show up.
        for (const auto* gs : {&nv->tGeoSet0, &nv->tGeoSet1}) {
            totalSubObjects += gs->arSubObjects.size();
            if (static_cast<i64>(gs->arSubObjects.size()) != gs->dwSubObjectCount) {
                if (mismatch < 5)
                    std::cout << "  GEOSET COUNT " << e.path().filename().string()
                              << ": span=" << gs->arSubObjects.size()
                              << " declared=" << gs->dwSubObjectCount << "\n";
                ++mismatch;
            }
            for (const auto& sm : gs->arSubObjects) {
                if (!sm.szName.empty()) ++namedMeshes;
                vertices += sm.arVertices.size();
                indices += sm.arIndices.size();
                influences += sm.arVertexInfluences.size();
                // A SubObject declares both counts; the resolved arrays must match.
                if (static_cast<i64>(sm.arVertices.size()) != sm.dwVertexCount ||
                    static_cast<i64>(sm.arIndices.size()) != sm.dwIndexCount) {
                    if (mismatch < 5)
                        std::cout << "  SUBOBJECT COUNT " << e.path().filename().string()
                                  << ": verts=" << sm.arVertices.size() << "/" << sm.dwVertexCount
                                  << " idx=" << sm.arIndices.size() << "/" << sm.dwIndexCount << "\n";
                    ++mismatch;
                }
            }
        }

        // The octree's three arrays each carry their own count word.
        const auto& oc = nv->tOctree;
        if (!oc.arNodes.empty()) ++filesWithOctree;
        octreeNodes += oc.arNodes.size();
        octreeLeaves += oc.arLeaves.size();
        octreePrims += oc.arPrimitives.size();
        if (static_cast<i64>(oc.arNodes.size()) != oc.dwNodeCount ||
            static_cast<i64>(oc.arLeaves.size()) != oc.dwLeafCount ||
            static_cast<i64>(oc.arPrimitives.size()) != oc.dwPrimitiveCount) {
            if (mismatch < 5)
                std::cout << "  OCTREE COUNT " << e.path().filename().string() << ": "
                          << oc.arNodes.size() << "/" << oc.dwNodeCount << " "
                          << oc.arLeaves.size() << "/" << oc.dwLeafCount << " "
                          << oc.arPrimitives.size() << "/" << oc.dwPrimitiveCount << "\n";
            ++mismatch;
        }

        // THE MaterialEntry FINDING: a material slot holds one variant per
        // look, so its variant count must equal dwLookCount in every file.
        for (const auto& m : nv->arMaterials) {
            if (!m.szName.empty()) ++namedMaterials;
            variants += m.arVariants.size();
            if (static_cast<i64>(m.arVariants.size()) != nv->dwLookCount) {
                if (variantLookMismatch < 5)
                    std::cout << "  VARIANT/LOOK " << e.path().filename().string() << " '"
                              << m.szName << "': variants=" << m.arVariants.size()
                              << " looks=" << nv->dwLookCount << "\n";
                ++variantLookMismatch;
            }
            for (const auto& v : m.arVariants) {
                tagEntries += v.arShaderParams.size();
                for (const auto& tg : v.arShaderParams)
                    if (tg.nValueType < 0 || tg.nValueType > 2) ++badTagType;
                texEntries += v.tMaterial.arTextures.size();
                if (v.tMaterial.snoShaderMap.valid()) ++shaderMapRefs;
            }
        }
    }

    std::cout << "\n=== native Appearance ===\n"
              << "files:        " << files << "\n"
              << "native parsed:" << nativeOk << "\n"
              << "compared:     " << compared << "\n"
              << "mismatches:   " << mismatch << "\n"
              << "bones:        " << totalBones << " (" << withBones << " files with bones)\n"
              << "sub-objects:  " << totalSubObjects << "\n"
              << "materials:    " << totalMaterials << "   looks: " << totalLooks << "\n"
              << "-- resolved content --\n"
              << "named bones:      " << namedBones << "\n"
              << "named meshes:     " << namedMeshes << "\n"
              << "named materials:  " << namedMaterials << "\n"
              << "named looks:      " << namedLooks << "\n"
              << "vertices:         " << vertices << "   indices: " << indices
              << "   influences: " << influences << "\n"
              << "capsules:         " << capsules << "   hardpoints: " << hardpoints << "\n"
              << "octree:           " << octreeNodes << " nodes, " << octreeLeaves
              << " leaves, " << octreePrims << " prims (" << filesWithOctree << " files)\n"
              << "material variants:" << variants << "   (count != lookCount: "
              << variantLookMismatch << ")\n"
              << "shader params:    " << tagEntries << " tag entries, bad type " << badTagType << "\n"
              << "textures:         " << texEntries << "   shaderMap refs: " << shaderMapRefs << "\n";

    CHECK(nativeOk == files);
    CHECK(compared > 0);
    CHECK(mismatch == 0);
    CHECK(totalBones > 0);
    CHECK(totalSubObjects > 0);
    // Names and geometry must actually come through, not just their descriptors.
    CHECK(namedBones == totalBones);
    CHECK(namedMeshes == totalSubObjects);
    CHECK(namedMaterials == totalMaterials);
    CHECK(namedLooks == totalLooks);
    CHECK(vertices > 0);
    CHECK(indices > 0);
    CHECK(influences > 0);
    CHECK(octreeNodes > 0);
    CHECK(hardpoints > 0);
    // One material variant per look, everywhere.
    CHECK(variants > 0);
    CHECK(variantLookMismatch == 0);
    // The embedded materials must resolve their tag maps and texture slots.
    CHECK(tagEntries > 0);
    CHECK(badTagType == 0);
    CHECK(texEntries > 0);
    CHECK(shaderMapRefs > 0);
}

/// The packed halves of a FatVertex. Nothing here trusts the decoders' word for
/// it: a normal that is not unit length, a tangent that does not follow the UV
/// gradient, or a cloth index that overruns the cloth it points into would all
/// mean the 44-byte layout is wrong.
TEST_CASE("D3 native: Appearance vertex packing", "[d3][native]") {
    auto base = findCorpus();
    if (base.empty()) SKIP("D3 corpus not found");
    const fs::path dir = base / "Appearances";
    if (!fs::is_directory(dir)) SKIP("Appearance corpus not found");

    i64 files = 0, verts = 0, tris = 0;
    i64 badNormal = 0, badTangent = 0, badBinormal = 0, badPadByte = 0;
    i64 clothSubs = 0, clothFlagWrong = 0, clothIndexOutOfRange = 0;
    i64 opaqueColors = 0, colorTotal = 0;
    f64 uvMin = 1e30, uvMax = -1e30;
    f64 tangentDot = 0.0, binormalDot = 0.0;
    i64 frames = 0;

    const auto len = [](const Vector3f& v) {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    };

    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".app") continue;
        if (++files > 400) break;
        auto bytes = readFile(e.path());
        auto nv = nat::parseAppearances(bytes);
        if (!nv) continue;

        for (const auto& sm : nv->tGeoSet0.arSubObjects) {
            const bool hasCloth = !sm.arClothData.empty();
            if (hasCloth) ++clothSubs;
            // The one provable format bit: 0x100 means "the vertex carries a
            // cloth index", and it tracks the presence of a ClothStructure.
            if (hasCloth != ((sm.dwVertexFormat & nat::kSubObjectHasClothIndex) != 0))
                ++clothFlagWrong;
            const i64 clothVerts =
                hasCloth ? static_cast<i64>(sm.arClothData[0].arVertices.size()) : 0;

            for (const auto& v : sm.arVertices) {
                ++verts;
                if (std::abs(len(nat::vertexNormal(v)) - 1.0f) > 0.05f) ++badNormal;
                if (std::abs(len(nat::vertexTangent(v)) - 1.0f) > 0.15f) ++badTangent;
                if (std::abs(len(nat::vertexBinormal(v)) - 1.0f) > 0.15f) ++badBinormal;
                // The 4th byte of each packed vector is unused and always zero.
                if ((v.dwNormal >> 24) || (v.dwTangent >> 24) || (v.dwBinormal >> 24))
                    ++badPadByte;
                if (hasCloth && static_cast<i64>(v.dwClothVertexIndex) >= clothVerts)
                    ++clothIndexOutOfRange;
                if (!hasCloth && v.dwClothVertexIndex != 0) ++clothIndexOutOfRange;
                auto uv = nat::unpackTexCoord(v.dwTexCoord0);
                uvMin = std::min({uvMin, f64(uv.x), f64(uv.y)});
                uvMax = std::max({uvMax, f64(uv.x), f64(uv.y)});
                ++colorTotal;
                if (nat::vertexColor(v).a == 255) ++opaqueColors;
            }

            // Rebuild the tangent frame from the triangle and compare. The
            // stored tangent follows +dP/du; the binormal follows -dP/dv.
            const auto& idx = sm.arIndices;
            const auto& vs = sm.arVertices;
            for (size_t t = 0; t + 2 < idx.size() && t < 60; t += 3) {
                const u32 i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
                if (i0 >= vs.size() || i1 >= vs.size() || i2 >= vs.size()) continue;
                const Vector3f p0 = vs[i0].vPosition, p1 = vs[i1].vPosition, p2 = vs[i2].vPosition;
                const Vector2f a0 = nat::unpackTexCoord(vs[i0].dwTexCoord0);
                const Vector2f a1 = nat::unpackTexCoord(vs[i1].dwTexCoord0);
                const Vector2f a2 = nat::unpackTexCoord(vs[i2].dwTexCoord0);
                const Vector3f e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
                const Vector3f e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
                const f32 du1 = a1.x - a0.x, dv1 = a1.y - a0.y;
                const f32 du2 = a2.x - a0.x, dv2 = a2.y - a0.y;
                const f32 det = du1 * dv2 - du2 * dv1;
                if (std::abs(det) < 1e-9f) continue;
                const f32 r = 1.0f / det;
                Vector3f T{(e1.x * dv2 - e2.x * dv1) * r, (e1.y * dv2 - e2.y * dv1) * r,
                           (e1.z * dv2 - e2.z * dv1) * r};
                Vector3f B{(e2.x * du1 - e1.x * du2) * r, (e2.y * du1 - e1.y * du2) * r,
                           (e2.z * du1 - e1.z * du2) * r};
                const f32 lt = len(T), lb = len(B);
                if (lt < 1e-6f || lb < 1e-6f) continue;
                const Vector3f st = nat::vertexTangent(vs[i0]);
                const Vector3f sb = nat::vertexBinormal(vs[i0]);
                tangentDot += (T.x * st.x + T.y * st.y + T.z * st.z) / lt;
                binormalDot += (B.x * sb.x + B.y * sb.y + B.z * sb.z) / lb;
                ++frames;
                ++tris;
            }
        }
    }

    std::cout << "\n=== native Appearance vertex packing ===\n"
              << "files " << files << "  vertices " << verts << "  triangles " << tris << "\n"
              << "non-unit normal/tangent/binormal: " << badNormal << " / " << badTangent << " / "
              << badBinormal << "   non-zero pad byte: " << badPadByte << "\n"
              << "cloth sub-objects " << clothSubs << "   format-bit 0x100 wrong "
              << clothFlagWrong << "   cloth index out of range " << clothIndexOutOfRange << "\n"
              << "texcoord0 range " << uvMin << " .. " << uvMax << "\n"
              << "mean cos(derived T, stored tangent)  " << (tangentDot / std::max<i64>(1, frames))
              << "\n"
              << "mean cos(derived B, stored binormal) " << (binormalDot / std::max<i64>(1, frames))
              << "\n"
              << "opaque vertex colours " << opaqueColors << " / " << colorTotal << "\n";

    CHECK(verts > 0);
    CHECK(frames > 0);
    // The packed vectors are unit length, near enough for 8 bits per component.
    CHECK(badNormal < verts / 1000);
    CHECK(badTangent < verts / 100);
    CHECK(badBinormal < verts / 100);
    CHECK(badPadByte == 0);
    // The cloth flag and the cloth index agree with the cloth block, exactly.
    CHECK(clothSubs > 0);
    CHECK(clothFlagWrong == 0);
    CHECK(clothIndexOutOfRange == 0);
    // A wrong UV scale would blow the range out. 8.8 fixed point spans exactly
    // [-64, +64 - 1/512], and the corpus reaches both ends of it.
    CHECK(uvMin >= -64.0);
    CHECK(uvMax <= 64.0 - 1.0 / 512.0);
    // Tangent follows +dP/du, binormal follows -dP/dv. Getting either the UV
    // decode or the tangent/binormal order wrong flips or zeroes these.
    CHECK(tangentDot / frames > 0.5);
    CHECK(binormalDot / frames < -0.5);
    CHECK(opaqueColors > colorTotal / 2);
}

/// Material: the standalone .mat carries the same `UberMaterial` body that an
/// .app embeds, and its shader-parameter block is a D3 TagMap -- a count-prefixed
/// run of 12-byte {valueType, tagId, value} entries.  Both are checked here
/// against the engine's own 23-entry tag table, whose ids no corpus file leaves.
TEST_CASE("D3 native: Material tag map and texture slots", "[d3][native]") {
    auto base = findCorpus();
    if (base.empty()) SKIP("D3 corpus not found");
    const fs::path dir = base / "Material";
    if (!fs::is_directory(dir)) SKIP("Material corpus not found");

    // The material tag namespace registered at 0x71010921F8 (23 entries), with
    // the value type the engine's own type code implies.
    struct Tag { u32 id; int valueType; };
    static const Tag kTags[] = {
        {0x30100, 1}, {0x30101, 1}, {0x30102, 1}, {0x30103, 0}, {0x30104, 0},
        {0x30105, 2}, {0x30106, 0}, {0x30107, 0},
        {0x30200, 2}, {0x30201, 2}, {0x30202, 2}, {0x30203, 2}, {0x30204, 2},
        {0x30300, 0}, {0x30301, 1}, {0x30302, 1}, {0x30303, 1},
        {0x30400, 2}, {0x30401, 2}, {0x30402, 2}, {0x30403, 2}, {0x30404, 2},
        {0x30405, 1},
    };

    size_t files = 0, parsed = 0, tags = 0, unknownTag = 0, wrongType = 0;
    size_t textures = 0, texRefs = 0, shaderMaps = 0, colorSets = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".mat") continue;
        ++files;
        auto data = readFile(e.path());
        auto nv = nat::parseMaterial(data);
        if (!nv) continue;
        ++parsed;

        for (const auto& t : nv->arShaderParams) {
            ++tags;
            const Tag* found = nullptr;
            for (const auto& k : kTags)
                if (k.id == t.dwTagId) { found = &k; break; }
            if (!found) { ++unknownTag; continue; }
            if (found->valueType != t.nValueType) ++wrongType;
        }
        if (nv->tMaterial.snoShaderMap.valid()) ++shaderMaps;
        if (nv->tMaterial.tColors.vDiffuse.x != 0.0f) ++colorSets;
        for (const auto& tx : nv->tMaterial.arTextures) {
            ++textures;
            if (tx.snoTexture.valid()) ++texRefs;
        }
    }

    std::cout << "\n=== native Material ===\n"
              << "files: " << files << "  parsed: " << parsed << "\n"
              << "shader-param tags: " << tags << "  unknown id: " << unknownTag
              << "  wrong value type: " << wrongType << "\n"
              << "textures: " << textures << " (" << texRefs << " with a texture ref)\n"
              << "shaderMap refs: " << shaderMaps << "  non-black diffuse: " << colorSets << "\n";

    CHECK(files > 0);
    CHECK(parsed == files);
    CHECK(tags > 0);
    // Every id in the corpus is one the engine registers, with the value type
    // its tag-table entry declares.
    CHECK(unknownTag == 0);
    CHECK(wrongType == 0);
    CHECK(textures > 0);
    // A slot may legitimately bind no texture: 11 of 16972 leave snoTexture unset.
    CHECK(texRefs > textures - textures / 100);
    CHECK(shaderMaps > 0);
}

/// PhysMesh stores the END of its mesh block rather than a length, so the
/// resolver subtracts.  Getting that backwards would read a wildly oversized
/// range, which `locate` would reject and leave the payload empty -- so a
/// non-empty payload on every file with a declared block is the real check.
/// PhysMesh carries Domino collision meshes: quantised vertices, triangles and
/// a BVH.  The BVH node word is the check that matters -- a 2-bit tag where 3
/// means leaf {count=(d>>2)&15, first=d>>6} and 0/1/2 is an internal split axis
/// with rightChild = idx + (d>>6).  If that decode is wrong the DFS either
/// revisits a node or leaves triangles uncovered, so tiling [0, triangleCount)
/// exactly is a proof that the whole structure was read correctly.
TEST_CASE("D3 native: PhysMesh collision meshes and BVH", "[d3][native]") {
    auto base = findCorpus();
    if (base.empty()) SKIP("D3 corpus not found");
    const fs::path dir = base / "PhysMesh";
    if (!fs::is_directory(dir)) SKIP("PhysMesh corpus not found");

    size_t files = 0, parsed = 0, meshes = 0, countMismatch = 0;
    size_t verts = 0, tris = 0, nodes = 0;
    size_t bvhChecked = 0, bvhBad = 0, apexOk = 0, apexTotal = 0, apexSentinel = 0;

    for (const auto& e : fs::directory_iterator(dir)) {
        if (files >= 1500) break;
        if (!e.is_regular_file() || e.path().extension() != ".phm") continue;
        ++files;
        auto data = readFile(e.path());
        if (data.size() < 32) continue;
        auto pm = nat::parsePhysMesh(data);
        if (!pm) continue;
        ++parsed;

        if (static_cast<i64>(pm->arMeshes.size()) != pm->dwMeshCount) ++countMismatch;
        for (const auto& cm : pm->arMeshes) {
            ++meshes;
            verts += cm.arVertices.size();
            tris += cm.arTriangles.size();
            nodes += cm.arNodes.size();
            if (static_cast<i64>(cm.arVertices.size()) != cm.dwVertexCount ||
                static_cast<i64>(cm.arTriangles.size()) != cm.dwTriangleCount ||
                static_cast<i64>(cm.arNodes.size()) != cm.dwNodeCount)
                ++countMismatch;

            // The apex-vertex reading of dmMeshTriangle: nOpposite[k] indexes a
            // vertex, not a triangle, so every non-negative entry must stay
            // inside the vertex array.  Negatives are the open-edge sentinel --
            // an edge with no neighbouring face has no apex -- so they are
            // counted separately rather than held against the reading.
            for (const auto& t : cm.arTriangles)
                for (i32 o : {t.nOpposite0, t.nOpposite1, t.nOpposite2}) {
                    if (o < 0) { ++apexSentinel; continue; }
                    ++apexTotal;
                    if (o < cm.dwVertexCount) ++apexOk;
                }

            if (cm.arNodes.empty() || cm.arTriangles.empty()) continue;
            ++bvhChecked;
            std::vector<u8> covered(cm.arTriangles.size(), 0);
            std::vector<u8> visited(cm.arNodes.size(), 0);
            std::vector<size_t> stack{0};
            bool ok = true;
            while (!stack.empty() && ok) {
                const size_t idx = stack.back();
                stack.pop_back();
                if (idx >= cm.arNodes.size() || visited[idx]) { ok = false; break; }
                visited[idx] = 1;
                const u32 d = static_cast<u32>(cm.arNodes[idx].dwData);
                if ((d & 3u) == 3u) {
                    const size_t first = d >> 6, count = (d >> 2) & 15u;
                    if (first + count > covered.size()) { ok = false; break; }
                    for (size_t k = 0; k < count; ++k) {
                        if (covered[first + k]) { ok = false; break; }
                        covered[first + k] = 1;
                    }
                } else {
                    stack.push_back(idx + 1);
                    stack.push_back(idx + (d >> 6));
                }
            }
            if (ok)
                for (u8 c : covered)
                    if (!c) { ok = false; break; }
            if (!ok) ++bvhBad;
        }
    }

    std::cout << "\n=== native PhysMesh ===\n"
              << "files " << files << "  parsed " << parsed << "  meshes " << meshes << "\n"
              << "vertices " << verts << "  triangles " << tris << "  bvh nodes " << nodes << "\n"
              << "declared-count mismatches " << countMismatch << "\n"
              << "BVH walks: " << bvhChecked << " checked, " << bvhBad << " failed to tile\n"
              << "apex vertex in range: " << apexOk << " / " << apexTotal << "\n";

    CHECK(parsed == files);
    CHECK(meshes > 0);
    CHECK(countMismatch == 0);
    CHECK(verts > 0);
    CHECK(tris > 0);
    // Every leaf range tiles the triangle array exactly, no node visited twice.
    CHECK(bvhChecked > 0);
    CHECK(bvhBad == 0);
    // nOpposite is a vertex index; a triangle-index reading would land out of
    // range constantly, so every non-sentinel entry must resolve.
    CHECK(apexTotal > 0);
    CHECK(apexOk == apexTotal);
}

/// Particle's 2296-byte struct is 40 inline animated channels (48 bytes each)
/// plus configuration.  The layout was re-derived from the engine's own path
/// struct, which puts the keyframe array's (offset,size) descriptor at the END
/// of a path rather than the start -- so a reader split on the old boundary
/// reads each header against the wrong array.
///
/// Two file-level invariants catch that, and neither can be satisfied by a
/// misaligned layout: a path's node array is a whole number of nodes, and the
/// triggered-event array's element count and byte length only agree at 412
/// bytes per element if both were read from the right offsets.
TEST_CASE("D3 native: Particle paths and payloads", "[d3][native]") {
    auto base = findCorpus();
    if (base.empty()) SKIP("D3 corpus not found");
    const fs::path dir = base / "Particle";
    if (!fs::is_directory(dir)) SKIP("Particle corpus not found");

    size_t files = 0, parsed = 0;
    size_t pathsWithKeys = 0, nodes = 0, ragged = 0;
    size_t textures = 0, shaderMaps = 0;
    size_t shapeNames = 0, events = 0, eventCountMismatch = 0;
    size_t physRefs = 0, actorRefs = 0;

    // Generic over the path type, so every channel is walked whatever its node
    // type is.
    auto scan = [&](const auto& path) {
        if (path.arNodes.empty()) return;
        ++pathsWithKeys;
        nodes += path.arNodes.size();
    };

    for (const auto& e : fs::directory_iterator(dir)) {
        if (files >= 4000) break;
        if (!e.is_regular_file() || e.path().extension() != ".prt") continue;
        ++files;
        auto data = readFile(e.path());
        if (data.size() < 32) continue;
        auto pt = nat::parseParticle(data);
        if (!pt) continue;
        ++parsed;

        scan(pt->arSizeScalePath);   scan(pt->arCountPath);
        scan(pt->arEffectScalePath); scan(pt->arParticleLifePath);
        scan(pt->arInitialSizePath); scan(pt->arSpreadAnglePath);
        scan(pt->arInitialVelocityPath); scan(pt->arInitialVelocityWorldPath);
        scan(pt->arEmissionRatePath); scan(pt->arEmitterRatePathA);
        scan(pt->arEmitterRatePathB); scan(pt->arUnknownEmitterVectorPath);
        scan(pt->arUnknownEmitterFloatPath);
        scan(pt->arColorPath);   scan(pt->arScalePath);   scan(pt->arAlphaPath);
        scan(pt->arSizePath);    scan(pt->arSize2Path);   scan(pt->arRotationPath);
        scan(pt->arRotationRatePath);  scan(pt->arRotation2RatePath);
        scan(pt->arRotation2Path);     scan(pt->arAxisPath);
        scan(pt->arOffsetPath);        scan(pt->arVelocityPath);
        scan(pt->arAccelerationPath);  scan(pt->arOffset2Path);
        scan(pt->arVelocity2Path);     scan(pt->arAcceleration2Path);
        scan(pt->tEmitter.tShapeExtent0); scan(pt->tEmitter.tShapeExtent1);
        scan(pt->tEmitter.tShapeExtent2);

        // A ragged array would have been rejected by `locate` and come back
        // short, so compare against the declared byte length in the file image.
        const auto& img = *reinterpret_cast<const nat::layout::Particle*>(data.data() + 16);
        auto bad = [&](i32 size, size_t elem) { return size > 0 && size % elem != 0; };
        if (bad(img.arSizeScalePath.arNodes_size, sizeof(nat::layout::FloatNode)) ||
            bad(img.arColorPath.arNodes_size, sizeof(nat::layout::ColorNode)) ||
            bad(img.arAxisPath.arNodes_size, sizeof(nat::layout::VectorNode)))
            ++ragged;

        // ParticleColorSet turned out to be the engine's UberMaterial: the
        // "colour gradient" is its MaterialTextureEntry array, which is why its
        // byte length was always a multiple of 160.
        if (pt->tMaterial.snoShaderMap.valid()) ++shaderMaps;
        textures += pt->tMaterial.arTextures.size();

        if (pt->snoPhysics.valid()) ++physRefs;
        if (pt->snoActor.valid()) ++actorRefs;
        if (!pt->tEmitter.szDccShapeName.empty()) ++shapeNames;
        if (pt->dwTriggeredEventCount > 0) {
            ++events;
            if (pt->arTriggeredEvents.size() !=
                static_cast<size_t>(pt->dwTriggeredEventCount))
                ++eventCountMismatch;
        }
    }

    std::cout << "\n=== native Particle ===\n"
              << "files:              " << files << "\n"
              << "parsed:             " << parsed << "\n"
              << "channels with keys: " << pathsWithKeys << "\n"
              << "keyframe nodes:     " << nodes << "\n"
              << "ragged node arrays: " << ragged << "\n"
              << "material texture slots: " << textures
              << "   shaderMap refs: " << shaderMaps << "\n"
              << "emitter shape names:" << shapeNames << "\n"
              << "physics refs:       " << physRefs << "   actor refs: " << actorRefs << "\n"
              << "triggered events:   " << events
              << " (count mismatch " << eventCountMismatch << ")\n";

    CHECK(parsed == files);
    CHECK(pathsWithKeys > 0);
    CHECK(nodes > 0);
    CHECK(ragged == 0);
    // The material body must resolve, or ParticleColorSet-is-UberMaterial is wrong.
    CHECK(shaderMaps > 0);
    CHECK(textures > 0);
    CHECK(shapeNames > 0);
    CHECK(events > 0);
    CHECK(eventCountMismatch == 0);
}

/// Anim's struct is 56 bytes and the animation data sits in the payload as an
/// array of 408-byte AnimPermutation.  The previous table modelled it as a
/// 448-byte struct holding one animation, which reads permutation 0 correctly
/// and silently drops the rest.
///
/// Every check here compares two fields the file stores separately, so a wrong
/// layout cannot satisfy them by construction: the permutation count against
/// the resolved array, and each permutation's bone count against the four
/// per-bone arrays that hang off it.
TEST_CASE("D3 native: Anim permutations", "[d3][native]") {
    auto base = findCorpus();
    if (base.empty()) SKIP("D3 corpus not found");
    const fs::path dir = base / "Anim";
    if (!fs::is_directory(dir)) SKIP("Anim corpus not found");

    size_t files = 0, parsed = 0, perms = 0, multiPerm = 0;
    size_t countMismatch = 0, boneMismatch = 0, curveMismatch = 0, rootMismatch = 0;
    size_t named = 0, bones = 0, attachments = 0, withAppearance = 0;
    size_t rootMotionOk = 0, rootMotionChecked = 0;
    size_t inPlaceSized = 0, inPlaceOk = 0, inPlaceChecked = 0, inPlacePopulated = 0;
    size_t quatKeys = 0, quatUnit = 0, keyCountBad = 0, keyOrderBad = 0;
    size_t densityOk = 0, densityChecked = 0;
    f32 worstQuat = 0.0f;

    for (const auto& e : fs::directory_iterator(dir)) {
        if (files >= 4000) break;
        if (!e.is_regular_file()) continue;
        ++files;
        auto data = readFile(e.path());
        if (data.size() < 32) continue;
        auto an = nat::parseAnim(data);
        if (!an) continue;
        ++parsed;

        if (static_cast<i64>(an->arPermutations.size()) != an->dwPermutationCount)
            ++countMismatch;
        if (an->dwPermutationCount > 1) ++multiPerm;
        if (an->snoAppearance.valid()) ++withAppearance;

        for (const auto& pm : an->arPermutations) {
            ++perms;
            if (!pm.szName.empty()) ++named;
            bones += pm.arBoneNames.size();
            attachments += pm.arAttachments.size();
            // one name and one translation/rotation/scale curve per bone
            if (static_cast<i64>(pm.arBoneNames.size()) != pm.dwBoneCount) ++boneMismatch;
            if (pm.arTranslationCurves.size() != pm.arBoneNames.size() ||
                pm.arRotationCurves.size() != pm.arBoneNames.size() ||
                pm.arScaleCurves.size() != pm.arBoneNames.size())
                ++curveMismatch;
            if (static_cast<i64>(pm.arAttachments.size()) != pm.dwAttachmentCount)
                ++rootMismatch;

            // The identity that pins both the root-motion array and the units of
            // vMovementVelocity: total displacement over the clip equals the
            // velocity times the clip's duration in ticks.
            if (pm.arRootMotion.size() >= 2 && pm.flFramesPerTick > 0.0f &&
                pm.dwFrameCount > 1) {
                ++rootMotionChecked;
                const auto& a = pm.arRootMotion.front();
                const auto& b = pm.arRootMotion.back();
                const f32 ticks = static_cast<f32>(pm.dwFrameCount - 1) / pm.flFramesPerTick;
                const f32 dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
                const f32 ex = pm.vMovementVelocity.x * ticks;
                const f32 ey = pm.vMovementVelocity.y * ticks;
                const f32 ez = pm.vMovementVelocity.z * ticks;
                const f32 tol = 0.01f + 0.01f * std::abs(ex) + 0.01f * std::abs(ey) +
                                0.01f * std::abs(ez);
                if (std::abs(dx - ex) <= tol && std::abs(dy - ey) <= tol &&
                    std::abs(dz - ez) <= tol)
                    ++rootMotionOk;
            }

            // The second per-frame track is the first one with the constant
            // velocity taken out -- "play it in place and let the game move
            // the actor". Both arrays always hold dwFrameCount entries.
            if (pm.arRootMotionInPlace.size() == pm.arRootMotion.size()) ++inPlaceSized;
            const size_t n = pm.arRootMotion.size();
            if (n >= 2 && pm.arRootMotionInPlace.size() == n) {
                const auto& last = pm.arRootMotion.back();
                bool populated = false;
                bool ok = true;
                for (size_t k = 0; k < n; ++k) {
                    const auto& tot = pm.arRootMotion[k];
                    const auto& ip = pm.arRootMotionInPlace[k];
                    if (ip.x != 0.0f || ip.y != 0.0f || ip.z != 0.0f) populated = true;
                    const f32 t = static_cast<f32>(k) / static_cast<f32>(n - 1);
                    if (std::abs((tot.x - last.x * t) - ip.x) > 1e-3f ||
                        std::abs((tot.y - last.y * t) - ip.y) > 1e-3f ||
                        std::abs((tot.z - last.z * t) - ip.z) > 1e-3f)
                        ok = false;
                }
                // Only the populated ones can actually distinguish the formula;
                // an all-zero pair satisfies it trivially.
                if (populated) {
                    ++inPlacePopulated;
                    ++inPlaceChecked;
                    if (ok) ++inPlaceOk;
                }
            }

            // Rotation keys are four SIGNED 16-bit components over 32767, not
            // the unsigned words the type registration declares.
            auto checkKeys = [&](auto& curves, auto keyOf) {
                for (const auto& c : curves) {
                    if (static_cast<i64>(c.arKeys.size()) != c.dwKeyCount) ++keyCountBad;
                    i32 prev = -1;
                    for (const auto& k : c.arKeys) {
                        if (keyOf(k) <= prev) ++keyOrderBad;
                        prev = keyOf(k);
                    }
                }
            };
            checkKeys(pm.arTranslationCurves, [](const auto& k) { return k.nFrame; });
            checkKeys(pm.arRotationCurves, [](const auto& k) { return k.nFrame; });
            checkKeys(pm.arScaleCurves, [](const auto& k) { return k.nFrame; });

            // The three floats at +0x50/54/58 are baked keyframe density, one
            // per channel: (float)(frames * bones) / (float)totalKeys. Asserted
            // bit-exactly -- the corpus reproduces that evaluation order on
            // 51,747 of 51,747 channel records, so any drift is a real defect
            // and not rounding. A channel with two keys per bone gives exactly
            // frames/2, which is why the scale slot reads that way in 97%.
            {
                const auto density = [&](const auto& curves) {
                    u64 keys = 0;
                    for (const auto& c : curves) keys += c.arKeys.size();
                    return keys == 0 ? 0.0f
                                     : static_cast<f32>(pm.dwFrameCount *
                                                        static_cast<i64>(pm.arBoneNames.size())) /
                                           static_cast<f32>(keys);
                };
                const auto cmp = [&](f32 stored, f32 pred) {
                    if (pred == 0.0f) return;
                    ++densityChecked;
                    if (stored == pred) ++densityOk;
                };
                cmp(pm.flFramesPerTranslationKey, density(pm.arTranslationCurves));
                cmp(pm.flFramesPerRotationKey, density(pm.arRotationCurves));
                cmp(pm.flFramesPerScaleKey, density(pm.arScaleCurves));
            }
            for (const auto& c : pm.arRotationCurves) {
                for (const auto& k : c.arKeys) {
                    ++quatKeys;
                    const auto comp = [](u16 v) {
                        return static_cast<f32>(static_cast<i16>(v)) / 32767.0f;
                    };
                    const f32 x = comp(k.tRotation.nX), y = comp(k.tRotation.nY);
                    const f32 z = comp(k.tRotation.nZ), w = comp(k.tRotation.nW);
                    const f32 len = std::sqrt(x * x + y * y + z * z + w * w);
                    worstQuat = std::max(worstQuat, std::abs(len - 1.0f));
                    if (std::abs(len - 1.0f) < 0.01f) ++quatUnit;
                }
            }
        }
    }

    std::cout << "\n=== native Anim ===\n"
              << "files:                " << files << "   parsed: " << parsed << "\n"
              << "permutations:         " << perms
              << "   (files with >1: " << multiPerm << ")\n"
              << "named permutations:   " << named << "\n"
              << "bone entries:         " << bones << "   attachments: " << attachments << "\n"
              << "appearance refs:      " << withAppearance << "\n"
              << "root-motion identity: " << rootMotionOk << " / " << rootMotionChecked << "\n"
              << "in-place residual:    " << inPlaceOk << " / " << inPlaceChecked
              << "   (populated tracks: " << inPlacePopulated << ")\n"
              << "rotation keys:        " << quatKeys << "   unit under i16/32767: "
              << quatUnit << "   worst |q|-1: " << worstQuat << "\n"
              << "-- cross-checks (all must be 0) --\n"
              << "permutation count:    " << countMismatch << "\n"
              << "bone count:           " << boneMismatch << "\n"
              << "curve counts:         " << curveMismatch << "\n"
              << "attachment count:     " << rootMismatch << "\n"
              << "key count vs array:   " << keyCountBad << "\n"
              << "key frames ascending: " << keyOrderBad << "\n"
              << "in-place track sized: " << (perms - inPlaceSized) << "\n"
              << "key density bit-exact:" << (densityChecked - densityOk) << " of "
              << densityChecked << "\n";

    CHECK(parsed == files);
    CHECK(perms > files);          // more permutations than files => multi-perm works
    CHECK(multiPerm > 0);
    CHECK(named == perms);
    CHECK(bones > 0);
    CHECK(countMismatch == 0);
    CHECK(boneMismatch == 0);
    CHECK(curveMismatch == 0);
    CHECK(rootMismatch == 0);
    // The root-motion arrays really are cumulative displacement.
    CHECK(rootMotionChecked > 0);
    CHECK(rootMotionOk > rootMotionChecked - rootMotionChecked / 50);
    // Both per-frame tracks are always present and the same length.
    CHECK(inPlaceSized == perms);
    // The in-place track is the total track minus the constant-velocity ramp.
    // All-zero pairs are excluded because they satisfy it trivially. It is
    // bit-exact on 32 of the 35 populated tracks corpus-wide; three clips are
    // hand-authored exceptions (see ANI spec §9.1), so this is the exporter's
    // rule, not a hard invariant. This case reads only the first 4000 files, so
    // assert the proportion rather than an absolute count.
    CHECK(inPlacePopulated > 0);
    CHECK(inPlaceOk >= inPlaceChecked - 3);
    // Curve keys: one per declared count, strictly ascending frames.
    CHECK(keyCountBad == 0);
    CHECK(keyOrderBad == 0);
    // The quaternion components are signed; reading them unsigned gives |q|~1.65.
    CHECK(quatKeys > 0);
    CHECK(quatUnit == quatKeys);
    // +0x50/54/58 are baked per-channel keyframe density, bit-exact in f32.
    CHECK(densityChecked > 0);
    CHECK(densityOk == densityChecked);
}

/// AnimSet is one core tag map plus a FixedArray of 28 override maps indexed by
/// the engine's WeaponClass enum (WEAPONCLASS_HTH = 0 ... ON_HORSE = 27).  Each
/// map is a TagMap: a 4-byte count then 12-byte {valueType, tagId, value}
/// entries, so `size == 4 + 12*count` is a layout proof.  Every override that
/// is set overrides a tag the core map already carries -- the array introduces
/// no new tags, which is what makes it an override layer rather than a peer.
TEST_CASE("D3 native: AnimSet weapon-class tag maps", "[d3][native]") {
    auto base = findCorpus();
    if (base.empty()) SKIP("D3 corpus not found");
    const fs::path dir = base / "AnimSet";
    if (!fs::is_directory(dir)) SKIP("AnimSet corpus not found");

    size_t files = 0, parsed = 0, ids = 0, populated = 0, withBase = 0, badId = 0;
    size_t countMismatch = 0, animRefs = 0, typeOther = 0;
    size_t overrides = 0, overridesInCore = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (files >= 3000) break;
        if (!e.is_regular_file()) continue;
        ++files;
        auto data = readFile(e.path());
        if (data.size() < 32) continue;
        auto as = nat::parseAnimSet(data);
        if (!as) continue;
        ++parsed;
        if (as->snoBaseAnimSet.valid()) ++withBase;

        const std::vector<nat::AnimSetTagMapEntry>* slots[] = {
            &as->tmHth, &as->tm1HSwing, &as->tm1HThrust, &as->tm2HSwing,
            &as->tm2HThrust, &as->tmStaff, &as->tmBow, &as->tmXBow, &as->tmWand,
            &as->tmDualWield, &as->tmHthWithOrb, &as->tm1HSwingWithOrb,
            &as->tm1HThrustWithOrb, &as->tmDualWieldSwordFist,
            &as->tmDualWieldFistFist, &as->tm1HFist, &as->tm2HAxeMace,
            &as->tmHandXBow, &as->tmWandWithOrb, &as->tm1HSwingWithShield,
            &as->tm1HThrustWithShield, &as->tmHthWithShield,
            &as->tm2HSwingWithShield, &as->tm2HThrustWithShield,
            &as->tmStaffWithShield, &as->tm2HFlail, &as->tm2HFlailWithShield,
            &as->tmOnHorse,
        };

        // The block stores its own element count as a 4-byte prefix.  Read it
        // straight out of the image and compare against what was resolved: the
        // two only agree if the descriptor and the stride are both right.
        const auto& img = *reinterpret_cast<const nat::layout::AnimSet*>(data.data() + 16);
        if (img.tCoreTagMap > 0 && img.tCoreTagMap_size >= 4) {
            i32 declared = 0;
            std::memcpy(&declared, data.data() + 16 + img.tCoreTagMap, sizeof(declared));
            if (static_cast<i64>(as->tCoreTagMap.size()) != declared) ++countMismatch;
            if (img.tCoreTagMap_size != 4 + 12 * declared) ++badId;
        }

        std::vector<i32> coreTags;
        coreTags.reserve(as->tCoreTagMap.size());
        for (const auto& en : as->tCoreTagMap) coreTags.push_back(en.dwTagId);
        std::sort(coreTags.begin(), coreTags.end());

        for (const auto& en : as->tCoreTagMap) {
            ++ids;
            if (en.snoAnim.valid()) ++animRefs;
            if (en.nValueType != 2) ++typeOther;
        }
        for (auto* s : slots) {
            if (!s->empty()) ++populated;
            for (const auto& en : *s) {
                ++ids;
                if (en.snoAnim.valid()) ++animRefs;
                if (en.nValueType != 2) ++typeOther;
                if (!en.snoAnim.valid()) continue;
                ++overrides;
                if (std::binary_search(coreTags.begin(), coreTags.end(), en.dwTagId))
                    ++overridesInCore;
            }
        }
    }
    std::cout << "\n=== native AnimSet ===\n"
              << "files: " << files << "   parsed: " << parsed << "\n"
              << "populated weapon-class maps: " << populated << "   entries: " << ids
              << "   with an anim ref: " << animRefs << "\n"
              << "entries with nValueType != 2: " << typeOther << "\n"
              << "base-set refs: " << withBase << "\n"
              << "overrides: " << overrides << "   also present in the core map: "
              << overridesInCore << "\n"
              << "count-prefix mismatch: " << countMismatch
              << "   size != 4+12*count: " << badId << "\n";
    CHECK(parsed == files);
    CHECK(populated > 0);
    CHECK(ids > 0);
    CHECK(withBase > 0);
    CHECK(countMismatch == 0);
    CHECK(badId == 0);
    // The weapon-class array is a pure override layer over the core map.
    CHECK(overrides > 0);
    CHECK(overridesInCore == overrides);
}

TEST_CASE("D3 native: AnimTree", "[d3][native]") {
    auto base = findCorpus();
    if (base.empty()) SKIP("D3 corpus not found");
    const fs::path dir = base / "AnimTree";
    if (!fs::is_directory(dir)) SKIP("AnimTree corpus not found");

    size_t files = 0, parsed = 0, leaves = 0, leafAnimRefs = 0, countMismatch = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        ++files;
        auto data = readFile(e.path());
        if (data.size() < 32) continue;
        auto at = nat::parseAnimTree(data);
        if (!at) continue;
        ++parsed;
        if (static_cast<i64>(at->arLeaves.size()) != at->dwLeafCount) ++countMismatch;
        leaves += at->arLeaves.size();
        for (const auto& lf : at->arLeaves)
            if (lf.snoAnim.valid()) ++leafAnimRefs;
    }
    std::cout << "\n=== native AnimTree ===\n"
              << "files: " << files << "   parsed: " << parsed << "\n"
              << "leaves: " << leaves << "   with an anim ref: " << leafAnimRefs
              << "   count mismatch: " << countMismatch << "\n";
    CHECK(parsed == files);
    CHECK(leaves > 0);
    CHECK(leafAnimRefs == leaves);
    CHECK(countMismatch == 0);
}

TEST_CASE("D3 native: speed vs the generic reader", "[d3][native][!benchmark]") {
    auto base = findCorpus();
    if (base.empty()) SKIP("D3 corpus not found");
    const fs::path dir = base / "Appearances";
    if (!fs::is_directory(dir)) SKIP("Appearances corpus not found");

    std::vector<std::vector<u8>> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (files.size() >= 1500) break;
        if (!e.is_regular_file() || e.path().extension() != ".app") continue;
        auto d = readFile(e.path());
        if (d.size() >= 32) files.push_back(std::move(d));
    }
    if (files.empty()) SKIP("no files");

    using clock = std::chrono::steady_clock;
    volatile i64 sink = 0;

    auto t0 = clock::now();
    for (const auto& d : files) {
        auto v = nat::parseAppearances(d);
        if (v) {
            sink += v->dwBoneCount;
            sink += static_cast<i64>(v->arBones.size());
            sink += static_cast<i64>(v->tGeoSet0.arSubObjects.size());
        }
    }
    auto t1 = clock::now();

    SnoReader reader;
    for (const auto& d : files) {
        auto g = reader.parse(d, SnoGroup::Appearance);
        if (g) {
            i64 n = 0;
            genericInt(g->root, "dwBoneCount", n);
            sink += n;
        }
    }
    auto t2 = clock::now();

    const double nativeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double genericMs = std::chrono::duration<double, std::milli>(t2 - t1).count();

    std::cout << "\n=== speed over " << files.size() << " Appearance files ===\n"
              << std::fixed << std::setprecision(2)
              << "native:  " << nativeMs << " ms\n"
              << "generic: " << genericMs << " ms\n"
              << "speedup: " << (nativeMs > 0 ? genericMs / nativeMs : 0) << "x\n";

    CHECK(sink != 0);
    CHECK(nativeMs < genericMs);
}
