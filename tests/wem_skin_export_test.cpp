// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// What a Warcraft III file holds for each vertex's skin (EDIT_MODE_SKIN_DESIGN.md
/// §12, gate S1): the Skin Quantizer, the 255 quantiser, `writtenSkin`, and the
/// corpus round trip that holds every shipped weight in place.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/mdx/writer.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/skinning/quantize.h>
#include <whiteout/models/wem/validate.h>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

// ============================================================================
// Reading a file's skin back, by bone name
// ============================================================================

using NameOf = std::unordered_map<u32, std::string>;

NameOf NamesByObjectId(const mdx::Model& model) {
    NameOf names;
    const auto take = [&](const auto& records) {
        for (const auto& record : records) {
            names.emplace(record.node.objectId, record.node.name);
        }
    };
    take(model.bones);
    take(model.helpers);
    take(model.attachments);
    take(model.lights);
    take(model.particleEmitters);
    take(model.particleEmitters2);
    take(model.ribbonEmitters);
    take(model.cornEmitters);
    take(model.eventObjects);
    take(model.collisionShapes);
    return names;
}

/// One vertex's skin as the file holds it.
struct VertexSkin {
    /// `SKIN`: (bone, byte) for every byte above zero, sorted.
    std::vector<std::pair<std::string, u16>> weighted;
    /// A matrix group: its bones, sorted, repeats kept.
    std::vector<std::string> group;
    bool sums255 = true;
};

std::string NameOrId(const NameOf& names, u32 objectId) {
    const auto it = names.find(objectId);
    return it != names.end() ? it->second : "#" + std::to_string(objectId);
}

bool UsesSkinChunk(const mdx::Model& model, const mdx::Geoset& geoset) {
    return model.version > 800 && !geoset.skinData.empty();
}

VertexSkin SkinOf(const mdx::Model& model, const mdx::Geoset& geoset, const NameOf& names,
                  std::size_t v) {
    VertexSkin out;
    if (UsesSkinChunk(model, geoset)) {
        u32 total = 0;
        for (std::size_t k = 0; k < 4 && v * 8 + 7 < geoset.skinData.size(); ++k) {
            const u16 byte = geoset.skinData[v * 8 + 4 + k];
            total += byte;
            if (byte == 0) {
                continue;
            }
            const u32 raw = geoset.skinData[v * 8 + k];
            const u32 objectId =
                raw < geoset.matrixIndices.size() ? geoset.matrixIndices[raw] : raw;
            out.weighted.emplace_back(NameOrId(names, objectId), byte);
        }
        out.sums255 = total == 255;
        std::sort(out.weighted.begin(), out.weighted.end());
        return out;
    }
    if (v >= geoset.vertexGroups.size()) {
        return out;
    }
    const u32 group = geoset.vertexGroups[v];
    u32 start = 0;
    for (u32 g = 0; g < group && g < geoset.matrixGroups.size(); ++g) {
        start += geoset.matrixGroups[g];
    }
    if (group >= geoset.matrixGroups.size()) {
        return out;
    }
    for (u32 k = 0; k < geoset.matrixGroups[group]; ++k) {
        if (start + k < geoset.matrixIndices.size()) {
            out.group.push_back(NameOrId(names, geoset.matrixIndices[start + k]));
        }
    }
    std::sort(out.group.begin(), out.group.end());
    return out;
}

/// The same skin with a bone named once: a file may name one in two `SKIN`
/// lanes, and the game blends the sum, so this changes nothing it draws.
std::vector<std::pair<std::string, u16>> Merged(std::vector<std::pair<std::string, u16>> weighted) {
    std::vector<std::pair<std::string, u16>> out;
    for (const auto& [name, byte] : weighted) {
        const auto same = std::find_if(out.begin(), out.end(),
                                       [&](const auto& kept) { return kept.first == name; });
        if (same != out.end()) {
            same->second = static_cast<u16>(same->second + byte);
        } else {
            out.emplace_back(name, byte);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool NamedTwice(const std::vector<std::pair<std::string, u16>>& weighted) {
    return Merged(weighted).size() != weighted.size();
}

bool HasRepeat(const std::vector<std::string>& sorted) {
    return std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end();
}

std::vector<std::string> AsSet(std::vector<std::string> names) {
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

/// The bones a vertex binds, as a set, whichever encoding holds them: a group
/// written as `SKIN` keeps its bones and gains its equal split's bytes.
std::vector<std::string> BonesOf(const VertexSkin& skin) {
    if (skin.weighted.empty()) {
        return AsSet(skin.group);
    }
    std::vector<std::string> names;
    for (const auto& entry : skin.weighted) {
        names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    return AsSet(std::move(names));
}

/// A vertex's identity across a round trip: its position, normal and first UV,
/// bit for bit. Import and export copy all three without arithmetic.
using VertexKey = std::array<u32, 8>;

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& key) const {
        std::size_t h = 1469598103934665603ull;
        for (const u32 word : key) {
            h = (h ^ word) * 1099511628211ull;
        }
        return h;
    }
};

VertexKey KeyOf(const mdx::Geoset& geoset, std::size_t v) {
    VertexKey key{};
    const auto bits = [](f32 value) {
        u32 word = 0;
        std::memcpy(&word, &value, sizeof(word));
        return word;
    };
    const Vector3f p = v < geoset.vertexPositions.size() ? geoset.vertexPositions[v] : Vector3f{};
    const Vector3f n = v < geoset.vertexNormals.size() ? geoset.vertexNormals[v] : Vector3f{};
    Vector2f uv{};
    if (!geoset.textureCoordinateSets.empty() && v < geoset.textureCoordinateSets[0].size()) {
        uv = geoset.textureCoordinateSets[0][v];
    }
    key = {bits(p.x), bits(p.y), bits(p.z), bits(n.x), bits(n.y), bits(n.z), bits(uv.x), bits(uv.y)};
    return key;
}

// ============================================================================
// The corpus arm's tally
// ============================================================================

struct SkinTally {
    u64 skinCompared = 0;
    u64 skinDiffering = 0;
    u64 skinSourceNot255 = 0;  ///< Differing, and the source's bytes did not sum to 255.
    u64 skinSourceMerged = 0;  ///< The source named a bone in two lanes; import merged them.
    u64 groupCompared = 0;
    u64 groupDiffering = 0;
    u64 groupDifferingBig = 0;    ///< Of those, the source group held 5 to 8 bones.
    u64 groupDifferingRepeat = 0; ///< Of those, the source group named a bone twice.
    /// Of those, a group of five to eight bones written as `SKIN`, which holds
    /// four: folded, as it must be.
    u64 groupFoldedToSkin = 0;
    u64 exportsRefused = 0;
    u64 encodingChanged = 0;      ///< `SKIN` in, groups out: a conversion, not compared.
    u64 unmatched = 0;            ///< No source vertex had the key.
    std::set<std::string> repeatGroups;
    std::vector<std::string> examples;

    void note(const std::string& example) {
        if (examples.size() < 12) {
            examples.push_back(example);
        }
    }

    u64 requiredSkin() const {
        return skinDiffering - skinSourceNot255;
    }
    u64 requiredGroups() const {
        return groupDiffering - groupDifferingRepeat - groupFoldedToSkin;
    }

    void print(const char* label) const {
        std::cout << "== " << label << "\n"
                  << "  SKIN vertices " << skinCompared << ", differing " << skinDiffering
                  << " (source not summing to 255: " << skinSourceNot255
                  << "; lanes merged: " << skinSourceMerged << ")\n"
                  << "  grouped vertices " << groupCompared << ", differing " << groupDiffering
                  << " (5-8 bone groups: " << groupDifferingBig
                  << ", repeat groups: " << groupDifferingRepeat
                  << ", folded into SKIN's four lanes: " << groupFoldedToSkin << ")\n"
                  << "  SKIN written as groups " << encodingChanged << ", unmatched " << unmatched
                  << ", exports refused " << exportsRefused << "\n";
        for (const std::string& example : examples) {
            std::cout << "    " << example << "\n";
        }
    }
};

std::string Describe(const VertexSkin& skin) {
    std::string text;
    if (!skin.weighted.empty()) {
        for (const auto& [name, byte] : skin.weighted) {
            text += name + ":" + std::to_string(byte) + " ";
        }
        return text;
    }
    text = "{";
    for (const std::string& name : skin.group) {
        text += name + " ";
    }
    return text + "}";
}

/// Compares every vertex @p exported writes for a mesh with the source geoset
/// the mesh was imported from.
void CompareModels(SkinTally& tally, const std::string& file, const mdx::Model& source,
                   const mdx::Model& exported, const MdxExportMap& map) {
    const NameOf sourceNames = NamesByObjectId(source);
    const NameOf exportedNames = NamesByObjectId(exported);
    for (std::size_t g = 0; g < source.geosets.size() && g < map.geosetsOfMesh.size(); ++g) {
        const mdx::Geoset& in = source.geosets[g];
        std::unordered_map<VertexKey, std::vector<u32>, VertexKeyHash> byKey;
        for (std::size_t v = 0; v < in.vertexPositions.size(); ++v) {
            byKey[KeyOf(in, v)].push_back(static_cast<u32>(v));
        }
        const bool inSkin = UsesSkinChunk(source, in);
        for (const u32 written : map.geosetsOfMesh[g]) {
            if (written >= exported.geosets.size()) {
                continue;
            }
            const mdx::Geoset& out = exported.geosets[written];
            const bool outSkin = UsesSkinChunk(exported, out);
            for (std::size_t v = 0; v < out.vertexPositions.size(); ++v) {
                const auto found = byKey.find(KeyOf(out, v));
                if (found == byKey.end()) {
                    ++tally.unmatched;
                    continue;
                }
                if (inSkin && !outSkin) {
                    ++tally.encodingChanged;
                    continue;
                }
                const VertexSkin got = SkinOf(exported, out, exportedNames, v);
                bool same = false;
                bool anyNot255 = false;
                bool anyMerged = false;
                bool anyBig = false;
                bool anyRepeat = false;
                VertexSkin first;
                for (const u32 candidate : found->second) {
                    const VertexSkin want = SkinOf(source, in, sourceNames, candidate);
                    if (candidate == found->second.front()) {
                        first = want;
                    }
                    anyNot255 = anyNot255 || !want.sums255;
                    anyBig = anyBig || AsSet(want.group).size() > 4;
                    if (HasRepeat(want.group)) {
                        anyRepeat = true;
                        tally.repeatGroups.insert(file + ": " + Describe(want));
                    }
                    // A bone named twice is compared as the one share the game
                    // blends, on both sides.
                    anyMerged = anyMerged || NamedTwice(want.weighted);
                    same = same || (inSkin ? Merged(want.weighted) == Merged(got.weighted)
                                           : AsSet(want.group) == BonesOf(got));
                }
                if (inSkin) {
                    ++tally.skinCompared;
                    tally.skinSourceMerged += anyMerged ? 1 : 0;
                } else {
                    ++tally.groupCompared;
                }
                if (same) {
                    continue;
                }
                if (inSkin) {
                    ++tally.skinDiffering;
                    tally.skinSourceNot255 += anyNot255 ? 1 : 0;
                } else {
                    ++tally.groupDiffering;
                    tally.groupDifferingBig += anyBig ? 1 : 0;
                    tally.groupDifferingRepeat += anyRepeat ? 1 : 0;
                    tally.groupFoldedToSkin += !anyRepeat && anyBig && outSkin ? 1 : 0;
                }
                if (!anyNot255 && !anyRepeat && !(anyBig && outSkin)) {
                    tally.note(file + " geoset " + std::to_string(g) + " vertex " +
                               std::to_string(v) + ": " + Describe(first) + " -> " +
                               Describe(got));
                }
            }
        }
    }
}

u32 VersionFor(ProfileId profile, u32 sourceVersion) {
    if (profile == ProfileId::Wc3Classic) {
        return 800;
    }
    return std::max<u32>(sourceVersion, 1000);
}

} // namespace

// ============================================================================
// Fixtures
// ============================================================================

namespace {

/// A document of @p nodeCount nodes, all bones from @p firstBone on and helpers
/// before it, carrying @p profiles, with one mesh: a triangle of its own per
/// three entries of @p skin, each vertex bound as its entry says.
Document SkinDocument(u32 nodeCount, u32 firstBone,
                      const std::vector<std::vector<geom::Influence>>& skin,
                      std::initializer_list<ProfileId> profiles,
                      std::optional<u32> rigidNode = std::nullopt) {
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "body";
    section.rigidNode = rigidNode;
    builder.addSection(std::move(section));
    const std::size_t count = (skin.size() + 2) / 3 * 3;
    for (std::size_t v = 0; v < count; ++v) {
        const u32 t = static_cast<u32>(v / 3);
        const u32 k = static_cast<u32>(v % 3);
        const geom::VertexId id = builder.addVertex(Vector3f{
            static_cast<f32>(t * 2 + (k == 1 ? 1 : 0)), static_cast<f32>(k == 2 ? 1 : 0), 0.0f});
        for (const geom::Influence& influence : skin[std::min(v, skin.size() - 1)]) {
            builder.addInfluence(id, influence.bone, influence.weight);
        }
    }
    for (u32 v = 0; v + 2 < count; v += 3) {
        const geom::FaceId face = builder.addTriangle(geom::VertexId(v), geom::VertexId(v + 1),
                                                      geom::VertexId(v + 2), 0);
        for (u32 corner = 0; corner < 3; ++corner) {
            builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
            builder.setCornerAttr(face, corner, geom::names::uv(0), Vector2f{0, 0});
        }
    }

    Document document;
    for (const ProfileId profile : profiles) {
        document.declare(profile);
    }
    document.defaultProfile = *profiles.begin();
    document.textures.push_back(TextureRef{});
    Model model;
    model.meshes.push_back(std::move(builder.build().mesh));
    model.addSlot("slot");
    for (const ProfileId profile : profiles) {
        ProfileMaterialSet set;
        set.profile = profile;
        set.looks.looks.push_back(Look{});
        set.resizeBindings(model.materialSlots.size());
        set.slotBindings[0].byLook[0] = 0;
        set.materials.push_back(Material{});
        model.profileSets.push_back(std::move(set));
    }
    for (u32 n = 0; n < nodeCount; ++n) {
        Node node;
        node.name = "node_" + std::to_string(n);
        node.kind = n >= firstBone ? NodeKind::Bone : NodeKind::Helper;
        node.parent = kInvalidNode;
        node.resetPayloadForKind();
        model.nodes.nodes.push_back(std::move(node));
    }
    document.models.push_back(std::move(model));
    return document;
}

std::vector<geom::Influence> Rigid(u32 node) {
    return {{node, 1.0f}};
}

std::vector<geom::Influence> Blend(std::initializer_list<std::pair<u32, f32>> pairs) {
    std::vector<geom::Influence> out;
    for (const auto& [node, weight] : pairs) {
        out.push_back({node, weight});
    }
    return out;
}

/// The node names @p influences bind, sorted.
std::vector<std::string> NamesOf(const Document& document,
                                 std::span<const geom::Influence> influences) {
    std::vector<std::string> names;
    for (const geom::Influence& influence : influences) {
        names.push_back(document.models[0].nodes.nodes[influence.bone].name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

/// Per vertex of the document's one mesh, keyed by position: the bone names the
/// exported file's geosets bind there.
std::map<std::pair<f32, f32>, std::vector<std::string>> BonesByPosition(const mdx::Model& file) {
    std::map<std::pair<f32, f32>, std::vector<std::string>> out;
    const NameOf names = NamesByObjectId(file);
    for (const mdx::Geoset& geoset : file.geosets) {
        for (std::size_t v = 0; v < geoset.vertexPositions.size(); ++v) {
            const Vector3f p = geoset.vertexPositions[v];
            out[{p.x, p.y}] = BonesOf(SkinOf(file, geoset, names, v));
        }
    }
    return out;
}

/// The expected bone names at each position of `SkinDocument(…, skin, …)`.
std::map<std::pair<f32, f32>, std::vector<std::string>>
ExpectedByPosition(const Document& document,
                   const std::vector<std::vector<geom::Influence>>& skin) {
    std::map<std::pair<f32, f32>, std::vector<std::string>> out;
    const std::size_t count = (skin.size() + 2) / 3 * 3;
    for (std::size_t v = 0; v < count; ++v) {
        const u32 t = static_cast<u32>(v / 3);
        const u32 k = static_cast<u32>(v % 3);
        const std::vector<geom::Influence>& influences = skin[std::min(v, skin.size() - 1)];
        out[{static_cast<f32>(t * 2 + (k == 1 ? 1 : 0)), static_cast<f32>(k == 2 ? 1 : 0)}] =
            NamesOf(document, influences);
    }
    return out;
}

bool SameGroups(const mdx::Geoset& a, const mdx::Geoset& b) {
    return a.vertexGroups == b.vertexGroups && a.matrixGroups == b.matrixGroups &&
           a.matrixIndices == b.matrixIndices;
}

skinning::ClassicSkin QuantizeOne(std::vector<geom::Influence> vertex,
                                  const skinning::ClassicLimits& limits = {}) {
    const std::vector<std::vector<geom::Influence>> vertices{std::move(vertex)};
    return skinning::QuantizeClassic(vertices, {}, limits);
}

std::vector<u32> GroupOfOnly(const skinning::ClassicSkin& skin) {
    REQUIRE(skin.groupOf.size() == 1);
    return skin.groups[skin.groupOf[0]];
}

} // namespace

// ============================================================================
// S1, the file
// ============================================================================

TEST_CASE("S1 the quantizer snaps each row of the design table", "[wem][skin][quantize]") {
    // D§12.2's table; A is node 0, B node 1, and so on.
    using Row = std::pair<std::vector<geom::Influence>, std::vector<u32>>;
    const std::vector<Row> rows = {
        {Blend({{0, 1.0f}}), {0}},
        {Blend({{0, 0.97f}, {1, 0.03f}}), {0}},
        {Blend({{0, 0.9f}, {1, 0.1f}}), {0}},
        {Blend({{0, 0.8f}, {1, 0.2f}}), {0}},
        {Blend({{0, 0.7f}, {1, 0.3f}}), {0, 1}},
        {Blend({{0, 0.6f}, {1, 0.4f}}), {0, 1}},
        {Blend({{0, 0.45f}, {1, 0.45f}, {2, 0.1f}}), {0, 1}},
        {Blend({{0, 0.5f}, {1, 0.3f}, {2, 0.2f}}), {0, 1, 2}},
        {Blend({{0, 0.125f}, {1, 0.125f}, {2, 0.125f}, {3, 0.125f}, {4, 0.125f}, {5, 0.125f},
                {6, 0.125f}, {7, 0.125f}}),
         {0, 1, 2, 3, 4, 5, 6, 7}},
    };
    for (const auto& [weights, group] : rows) {
        CAPTURE(weights.size(), weights.front().weight);
        CHECK(GroupOfOnly(QuantizeOne(weights)) == group);
    }
    // The heavier bone is not always the lower node.
    CHECK(GroupOfOnly(QuantizeOne(Blend({{7, 0.9f}, {3, 0.1f}}))) == std::vector<u32>{7});
}

TEST_CASE("S1 the quantizer crosses over at 0.75", "[wem][skin][quantize]") {
    CHECK(GroupOfOnly(QuantizeOne(Blend({{0, 0.76f}, {1, 0.24f}}))) == std::vector<u32>{0});
    CHECK(GroupOfOnly(QuantizeOne(Blend({{0, 0.74f}, {1, 0.26f}}))) == std::vector<u32>{0, 1});
}

TEST_CASE("S1 the quantizer is idempotent on equal splits", "[wem][skin][quantize]") {
    for (u32 m = 1; m <= 8; ++m) {
        std::vector<geom::Influence> split;
        std::vector<u32> group;
        for (u32 b = 0; b < m; ++b) {
            split.push_back({b * 3, 1.0f / static_cast<f32>(m)});
            group.push_back(b * 3);
        }
        CAPTURE(m);
        const skinning::ClassicSkin skin = QuantizeOne(split);
        CHECK(GroupOfOnly(skin) == group);
        CHECK(skin.snapped == 0u);
    }
    // Nine equal bones hold more than a group can: the heaviest eight, by node.
    std::vector<geom::Influence> nine;
    for (u32 b = 0; b < 9; ++b) {
        nine.push_back({b, 1.0f / 9.0f});
    }
    CHECK(GroupOfOnly(QuantizeOne(nine)).size() == 8u);
}

TEST_CASE("S1 the quantizer never repeats a bone", "[wem][skin][quantize]") {
    // A bone named twice is one share of the summed weight: {A: 0.7, B: 0.3}.
    const skinning::ClassicSkin skin = QuantizeOne(Blend({{0, 0.4f}, {0, 0.3f}, {1, 0.3f}}));
    const std::vector<u32> group = GroupOfOnly(skin);
    CHECK(group == std::vector<u32>{0, 1});
    CHECK(std::adjacent_find(group.begin(), group.end()) == group.end());
}

TEST_CASE("S1 the quantizer drops bleed and binds an empty vertex to the first bone",
          "[wem][skin][quantize]") {
    // 0.015 is under the prune, so {A, B, C} snaps over {A, B} alone.
    CHECK(GroupOfOnly(QuantizeOne(Blend({{0, 0.5f}, {1, 0.485f}, {2, 0.015f}}))) ==
          std::vector<u32>{0, 1});
    const std::vector<std::vector<geom::Influence>> vertices{{}, Rigid(4), {}};
    const skinning::ClassicSkin skin = skinning::QuantizeClassic(vertices, {});
    REQUIRE(skin.groups.size() == 1u);
    CHECK(skin.groups[0] == std::vector<u32>{4});
    CHECK(skin.groupOf == std::vector<u32>{0, 0, 0});
    // Nothing bound anywhere writes no group at all.
    const std::vector<std::vector<geom::Influence>> none{{}, {}};
    CHECK(skinning::QuantizeClassic(none, {}).groups.empty());

    // The prune decides no group on its own -- a share under 0.02 always costs
    // more to keep than to drop, whatever the vertex holds -- so what it does
    // is keep bleed out of the eight the cap allows, and out of the count.
    std::vector<geom::Influence> eightAndBleed;
    for (u32 b = 0; b < 8; ++b) {
        eightAndBleed.push_back({b, 0.1225f});
    }
    eightAndBleed.push_back({8, 0.01f});
    eightAndBleed.push_back({9, 0.008f});
    const skinning::ClassicSkin wide = QuantizeOne(eightAndBleed);
    CHECK(GroupOfOnly(wide).size() == 8u);
    CHECK(wide.wide == 0u);
    // Nine real shares do not fit, and say so.
    std::vector<geom::Influence> nine;
    for (u32 b = 0; b < 9; ++b) {
        nine.push_back({b, 1.0f / 9.0f});
    }
    CHECK(QuantizeOne(nine).wide == 1u);
}

namespace {

/// @p groups distinct two-bone groups, each used by two or three vertices.
///
/// A group's vertices lean on DIFFERENT third bones -- 0.45 / 0.45 / 0.10 still
/// snaps to the two leads -- so when the group is merged away each of them has
/// its own nearest survivor. Moving them together would land some of them
/// somewhere worse, which is exactly what this fixture is for.
std::vector<std::vector<geom::Influence>> ManyGroups(u32 groups) {
    std::vector<std::vector<geom::Influence>> vertices;
    u32 made = 0;
    for (u32 a = 0; made < groups; ++a) {
        for (u32 b = a + 1; b < 40 && made < groups; ++b, ++made) {
            vertices.push_back(Blend({{a, 0.6f}, {b, 0.4f}}));
            for (u32 k = 0; k < 1 + made % 2; ++k) {
                const u32 third = (a + 1 + k * 7) % 40;
                if (third == a || third == b) {
                    vertices.push_back(Blend({{a, 0.55f}, {b, 0.45f}}));
                    continue;
                }
                vertices.push_back(Blend({{a, 0.45f}, {b, 0.45f}, {third, 0.10f}}));
            }
        }
    }
    return vertices;
}

/// `GroupCost`'s measure, recomputed here: the squared error of a group's equal
/// split against a vertex's normalised weights, less the constant `Σw²`.
f32 CostOf(std::span<const geom::Influence> vertex, const std::vector<u32>& group) {
    f32 total = 0.0f;
    for (const geom::Influence& influence : vertex) {
        total += influence.weight;
    }
    f32 inside = 0.0f;
    for (const geom::Influence& influence : vertex) {
        if (std::find(group.begin(), group.end(), influence.bone) != group.end()) {
            inside += influence.weight / total;
        }
    }
    const f32 k = static_cast<f32>(group.size());
    return (1.0f - 2.0f * inside) / k;
}

} // namespace

TEST_CASE("S1 the quantizer merges 300 groups down to 256", "[wem][skin][quantize]") {
    const std::vector<std::vector<geom::Influence>> vertices = ManyGroups(300);
    const skinning::ClassicSkin unlimited =
        skinning::QuantizeClassic(vertices, {}, skinning::ClassicLimits{8, 0.02f, 100000});
    REQUIRE(unlimited.groups.size() == 300u);

    const skinning::ClassicSkin skin = skinning::QuantizeClassic(vertices, {});
    CHECK(skin.groups.size() == 256u);
    CHECK(skin.mergedGroups == 44u);
    CHECK(skin.merged > 0u);
    REQUIRE(skin.groupOf.size() == vertices.size());
    u32 moved = 0;
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        REQUIRE(skin.groupOf[v] < skin.groups.size());
        const std::vector<u32>& own = unlimited.groups[unlimited.groupOf[v]];
        const std::vector<u32>& now = skin.groups[skin.groupOf[v]];
        if (own == now) {
            continue;
        }
        ++moved;
        // A moved vertex lands in the survivor nearest ITS OWN weights, which
        // a group-wide move cannot promise.
        f32 best = std::numeric_limits<f32>::max();
        for (const std::vector<u32>& group : skin.groups) {
            best = std::min(best, CostOf(vertices[v], group));
        }
        CHECK(CostOf(vertices[v], now) <= best + 1e-5f);
    }
    CHECK(moved == skin.merged);
    // Every survivor is still used by some vertex, and no two are equal.
    std::set<u32> used(skin.groupOf.begin(), skin.groupOf.end());
    CHECK(used.size() == skin.groups.size());
    std::set<std::vector<u32>> distinct(skin.groups.begin(), skin.groups.end());
    CHECK(distinct.size() == skin.groups.size());
}

TEST_CASE("S1 the quantizer keeps exactly 256 groups as they are", "[wem][skin][quantize]") {
    const std::vector<std::vector<geom::Influence>> vertices = ManyGroups(256);
    const skinning::ClassicSkin skin = skinning::QuantizeClassic(vertices, {});
    CHECK(skin.groups.size() == 256u);
    CHECK(skin.mergedGroups == 0u);
    CHECK(skin.merged == 0u);
}

TEST_CASE("S1 SKIN bytes sum to 255 and a byte comes back as itself", "[wem][skin][quantize]") {
    CHECK(skinning::QuantizeWeights({0.5f, 0.5f, 0.0f, 0.0f}, 2) ==
          std::array<u8, 4>{128, 127, 0, 0});
    for (u32 a = 0; a <= 255; ++a) {
        const f32 wa = static_cast<f32>(a) / 255.0f;
        const f32 wb = static_cast<f32>(255 - a) / 255.0f;
        const std::array<u8, 4> bytes = skinning::QuantizeWeights({wa, wb, 0.0f, 0.0f}, 2);
        CAPTURE(a);
        CHECK(bytes[0] == a);
        CHECK(bytes[1] == 255 - a);
    }
    for (u32 a = 0; a <= 255; a += 3) {
        for (u32 b = 0; a + b <= 255; b += 7) {
            for (u32 c = 0; a + b + c <= 255; c += 11) {
                const u32 d = 255 - a - b - c;
                const std::array<f32, 4> weights{
                    static_cast<f32>(a) / 255.0f, static_cast<f32>(b) / 255.0f,
                    static_cast<f32>(c) / 255.0f, static_cast<f32>(d) / 255.0f};
                const std::array<u8, 4> bytes = skinning::QuantizeWeights(weights, 4);
                CAPTURE(a, b, c, d);
                CHECK(bytes == std::array<u8, 4>{static_cast<u8>(a), static_cast<u8>(b),
                                                 static_cast<u8>(c), static_cast<u8>(d)});
            }
        }
    }
    // Weights that do not sum to one still write 255.
    const std::array<u8, 4> partial = skinning::QuantizeWeights({0.3f, 0.2f, 0.1f, 0.0f}, 3);
    CHECK(partial[0] + partial[1] + partial[2] == 255);

    // And through the export: every vertex sums to 255, 0.5/0.5 writes 128/127.
    const Document document =
        SkinDocument(4, 0,
                     {Blend({{0, 0.5f}, {1, 0.5f}}), Blend({{0, 0.3f}, {1, 0.2f}, {2, 0.2f}}),
                      Blend({{0, 0.6f}, {1, 0.3f}, {2, 0.1f}})},
                     {ProfileId::Wc3Reforged});
    const MdxConverter converter;
    const Result<mdx::Model> file = converter.toMdx(document, ProfileId::Wc3Reforged, 1000);
    REQUIRE(file.ok());
    REQUIRE(file->geosets.size() == 1u);
    const mdx::Geoset& geoset = file->geosets[0];
    for (std::size_t v = 0; v < geoset.vertexPositions.size(); ++v) {
        u32 total = 0;
        for (u32 k = 0; k < 4; ++k) {
            total += geoset.skinData[v * 8 + 4 + k];
        }
        CHECK(total == 255u);
    }
    CHECK(geoset.skinData[4] == 128);
    CHECK(geoset.skinData[5] == 127);
}

TEST_CASE("S1 node indices past 255 reach the right bones", "[wem][skin][export]") {
    const std::vector<std::vector<geom::Influence>> skin = {
        Rigid(5),   Rigid(5),   Rigid(5),   Rigid(255), Rigid(255),
        Rigid(255), Rigid(256), Rigid(256), Rigid(256), Rigid(299),
        Rigid(299), Rigid(299), Blend({{255, 0.5f}, {256, 0.5f}}),
    };
    const Document document =
        SkinDocument(300, 0, skin, {ProfileId::Wc3Reforged, ProfileId::Wc3Classic});
    const auto expected = ExpectedByPosition(document, skin);
    const MdxConverter converter;
    for (const auto& [profile, version] :
         {std::pair{ProfileId::Wc3Reforged, 1000u}, std::pair{ProfileId::Wc3Classic, 800u}}) {
        CAPTURE(version);
        const Result<mdx::Model> file = converter.toMdx(document, profile, version);
        REQUIRE(file.ok());
        CHECK(BonesByPosition(*file) == expected);
    }
}

TEST_CASE("S1 node indices past 255 reach the right bones in m3 and m2",
          "[wem][skin][export]") {
    // `.m3`: exported and read back, every vertex binds the same names.
    {
        const std::vector<std::vector<geom::Influence>> skin = {
            Rigid(5), Rigid(255), Rigid(256), Rigid(299), Blend({{255, 0.5f}, {256, 0.5f}}),
            Rigid(299)};
        const Document document = SkinDocument(300, 0, skin, {ProfileId::Sc2});
        const M3Converter converter;
        const Result<m3::Model> file = converter.toM3(document, ProfileId::Sc2);
        REQUIRE(file.ok());
        const Result<Document> back = converter.fromM3(*file, ProfileId::Sc2);
        REQUIRE(back.ok());
        const Model& model = back->models[0];
        std::map<std::vector<std::string>, u32> seen;
        for (const Mesh& mesh : model.meshes) {
            for (u32 v = 0; v < mesh.skin.vertexCount(); ++v) {
                ++seen[NamesOf(*back, mesh.skin.forVertex(v))];
            }
        }
        for (const std::vector<geom::Influence>& influences : skin) {
            CAPTURE(influences.front().bone);
            CHECK(seen.count(NamesOf(document, influences)) == 1u);
        }
    }
    // `.m2`: only bones become bones, so node 256 is the 57th bone when the
    // first 200 nodes are helpers. A byte index would have named a helper.
    {
        const std::vector<std::vector<geom::Influence>> skin = {Rigid(200), Rigid(255),
                                                                Rigid(256), Rigid(299)};
        const Document document = SkinDocument(300, 200, skin, {ProfileId::Wow});
        const M2Converter converter;
        const Result<m2::Model> file = converter.toM2(document, ProfileId::Wow);
        REQUIRE(file.ok());
        std::set<u32> bones;
        for (const m2::Vertex& vertex : file->vertices) {
            CHECK(vertex.boneWeights[0] == 255);
            bones.insert(vertex.boneIndices[0]);
        }
        CHECK(bones == std::set<u32>{0, 55, 56, 99});
    }
}

TEST_CASE("S1 the classic preview is the classic file", "[wem][skin][export]") {
    const std::vector<std::vector<geom::Influence>> skin = {
        Rigid(1),
        Blend({{1, 0.9f}, {2, 0.1f}}),
        Blend({{1, 0.6f}, {2, 0.4f}}),
        Blend({{0, 0.5f}, {1, 0.3f}, {2, 0.2f}}),
        Blend({{0, 0.2f}, {1, 0.2f}, {2, 0.2f}, {3, 0.2f}, {4, 0.2f}}),
        Blend({{3, 0.45f}, {4, 0.45f}, {1, 0.1f}}),
    };
    const Document document =
        SkinDocument(6, 0, skin, {ProfileId::Wc3Reforged, ProfileId::Wc3Classic});
    const MdxConverter converter;
    const Result<mdx::Model> preview =
        converter.toMdx(document, ProfileId::Wc3Reforged, 1000, ProfileId::Wc3Classic);
    const Result<mdx::Model> classic = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(preview.ok());
    REQUIRE(classic.ok());
    REQUIRE(preview->geosets.size() == classic->geosets.size());
    for (std::size_t g = 0; g < preview->geosets.size(); ++g) {
        CAPTURE(g);
        CHECK(preview->geosets[g].skinData.empty());
        CHECK_FALSE(preview->geosets[g].matrixGroups.empty());
        CHECK(SameGroups(preview->geosets[g], classic->geosets[g]));
    }
    // Everything else stays Reforged.
    CHECK(preview->version == 1000u);
}

namespace {

/// What @p file holds for each geoset vertex, as (node name, byte) with the
/// byte 0 for a group, sorted: the same shape `WrittenInfluence` names.
std::vector<std::vector<std::vector<std::pair<std::string, u16>>>>
FileSkin(const mdx::Model& file) {
    std::vector<std::vector<std::vector<std::pair<std::string, u16>>>> out;
    const NameOf names = NamesByObjectId(file);
    for (const mdx::Geoset& geoset : file.geosets) {
        auto& vertices = out.emplace_back();
        for (std::size_t v = 0; v < geoset.vertexPositions.size(); ++v) {
            const VertexSkin skin = SkinOf(file, geoset, names, v);
            auto& entry = vertices.emplace_back();
            if (!skin.weighted.empty() || UsesSkinChunk(file, geoset)) {
                entry = skin.weighted;
            } else {
                for (const std::string& name : skin.group) {
                    entry.emplace_back(name, 0);
                }
            }
            std::sort(entry.begin(), entry.end());
        }
    }
    return out;
}

std::vector<std::vector<std::vector<std::pair<std::string, u16>>>>
WrittenAsFile(const Document& document, const WrittenSkin& written) {
    std::vector<std::vector<std::vector<std::pair<std::string, u16>>>> out;
    for (const WrittenGeosetSkin& geoset : written.geosets) {
        auto& vertices = out.emplace_back();
        for (const std::vector<WrittenInfluence>& influences : geoset.influences) {
            auto& entry = vertices.emplace_back();
            for (const WrittenInfluence& influence : influences) {
                entry.emplace_back(document.models[0].nodes.nodes[influence.node].name,
                                   influence.byte);
            }
            std::sort(entry.begin(), entry.end());
        }
    }
    return out;
}

} // namespace

TEST_CASE("S1 writtenSkin agrees with the file", "[wem][skin][export]") {
    const std::vector<std::vector<geom::Influence>> skin = {
        Rigid(1),
        Blend({{1, 0.9f}, {2, 0.1f}}),
        Blend({{1, 0.6f}, {2, 0.4f}}),
        Blend({{0, 0.5f}, {1, 0.3f}, {2, 0.2f}}),
        Blend({{0, 0.2f}, {1, 0.2f}, {2, 0.2f}, {3, 0.2f}, {4, 0.2f}, {5, 0.0f}}),
        Blend({{3, 0.45f}, {4, 0.45f}, {1, 0.1f}}),
    };
    struct Fixture {
        const char* name;
        ProfileId profile;
        u32 version;
        std::optional<u32> rigid;
    };
    const Fixture fixtures[] = {
        {"classic", ProfileId::Wc3Classic, 800, std::nullopt},
        {"reforged", ProfileId::Wc3Reforged, 1000, std::nullopt},
        {"rigid section", ProfileId::Wc3Reforged, 1000, 4u},
    };
    const MdxConverter converter;
    for (const Fixture& fixture : fixtures) {
        CAPTURE(fixture.name);
        const Document document = SkinDocument(6, 0, skin, {fixture.profile}, fixture.rigid);
        const Result<mdx::Model> file =
            converter.toMdx(document, fixture.profile, fixture.version);
        REQUIRE(file.ok());
        // Through the bytes and back, as a reader sees them.
        const std::vector<u8> bytes = mdx::Writer().write(*file);
        const mdx::Model parsed = mdx::Parser().parse(std::span<const u8>(bytes.data(), bytes.size()));
        const WrittenSkin written = converter.writtenSkin(document, 0, fixture.profile, 0);
        CHECK(written.classic == (fixture.profile == ProfileId::Wc3Classic));
        CHECK(WrittenAsFile(document, written) == FileSkin(parsed));
        // And its vertices are the export map's.
        const std::vector<std::vector<u32>> vertices =
            MdxGeosetVertices(document, 0, fixture.profile);
        REQUIRE(vertices.size() == written.geosets.size());
        for (std::size_t g = 0; g < vertices.size(); ++g) {
            CHECK(vertices[g] == written.geosets[g].vertices);
        }
    }
    // A five-bone vertex is over Reforged's four, and within classic's eight.
    const Document document = SkinDocument(6, 0, skin, {ProfileId::Wc3Reforged});
    CHECK(converter.writtenSkin(document, 0, ProfileId::Wc3Reforged, 0).overLimit == 1u);
    CHECK(converter.writtenSkin(document, 0, ProfileId::Wc3Reforged, 0, ProfileId::Wc3Classic)
              .overLimit == 0u);
}

TEST_CASE("S1 a classic geoset keeps a group of five to eight bones", "[wem][skin][export]") {
    // Blizzard's own SD art names up to eight bones in one group, so nothing
    // folds to four before the quantizer decides (D§12.1).
    std::vector<geom::Influence> wide;
    for (u32 b = 0; b < 8; ++b) {
        wide.push_back({b, 0.125f});
    }
    std::vector<geom::Influence> five;
    for (u32 b = 0; b < 5; ++b) {
        five.push_back({b, 0.2f});
    }
    const std::vector<std::vector<geom::Influence>> skin = {wide, five, wide};
    const Document document = SkinDocument(8, 0, skin, {ProfileId::Wc3Classic});
    const MdxConverter converter;
    const Result<mdx::Model> file = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(file.ok());
    REQUIRE(file->geosets.size() == 1u);
    CHECK(file->geosets[0].matrixGroups == std::vector<u32>{8, 5});
    CHECK(BonesByPosition(*file) == ExpectedByPosition(document, skin));
    // And the profile says eight, so `Validate` does not call it a loss.
    CHECK(Validate(document, ValidateLevel::Profile).countOf(DiagCode::BoneInfluenceLimit) == 0u);
    CHECK(converter.writtenSkin(document, 0, ProfileId::Wc3Classic, 0).overLimit == 0u);
}

TEST_CASE("S1 import merges a bone named twice", "[wem][skin][import]") {
    mdx::Model model;
    model.version = 800;
    mdx::Bone a;
    a.node.name = "A";
    a.node.objectId = 0;
    a.node.parentId = mdx::Node::NO_PARENT;
    mdx::Bone b = a;
    b.node.name = "B";
    b.node.objectId = 1;
    model.bones = {a, b};
    model.pivotPoints = {Vector3f{0, 0, 0}, Vector3f{0, 0, 0}};
    mdx::Material material;
    material.layers.emplace_back();
    model.materials.push_back(material);
    mdx::Geoset geoset;
    geoset.vertexPositions = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{0, 1, 0}};
    geoset.vertexNormals.assign(3, Vector3f{0, 0, 1});
    geoset.textureCoordinateSets.push_back(std::vector<Vector2f>(3, Vector2f{0, 0}));
    geoset.faces = {0, 1, 2};
    geoset.faceTypeGroups = {4};
    geoset.faceGroups = {3};
    geoset.matrixGroups = {3};
    geoset.matrixIndices = {0, 0, 1};
    geoset.vertexGroups = {0, 0, 0};
    model.geosets.push_back(geoset);

    const Result<Document> document = MdxConverter().fromMdx(model);
    REQUIRE(document.ok());
    const geom::SkinBinding& skin = document->models[0].meshes[0].skin;
    for (u32 v = 0; v < skin.vertexCount(); ++v) {
        const std::span<const geom::Influence> influences = skin.forVertex(v);
        REQUIRE(influences.size() == 2u);
        CHECK(document->models[0].nodes.nodes[influences[0].bone].name == "A");
        CHECK(std::abs(influences[0].weight - 2.0f / 3.0f) < 1e-6f);
        CHECK(std::abs(influences[1].weight - 1.0f / 3.0f) < 1e-6f);
    }
}

TEST_CASE("S1 Validate says what a skin may hold", "[wem][skin][validate]") {
    const std::vector<std::vector<geom::Influence>> skin = {Rigid(1), Rigid(2), Rigid(2)};
    const auto errorsOf = [](const Diagnostics& report, DiagCode code) {
        u32 count = 0;
        for (const Diagnostic& d : report.all()) {
            count += d.code == code && d.severity == Severity::Error ? 1 : 0;
        }
        return count;
    };
    // A helper influence is written by the export: a warning, not an error.
    {
        Document document = SkinDocument(3, 2, skin, {ProfileId::Wc3Classic});
        const Diagnostics report = Validate(document, ValidateLevel::Structural);
        CHECK(report.countOf(DiagCode::DanglingNodeReference) == 1u);
        CHECK(errorsOf(report, DiagCode::DanglingNodeReference) == 0u);
    }
    // One outside the tree is an error.
    {
        Document document = SkinDocument(3, 0, skin, {ProfileId::Wc3Classic});
        document.models[0].meshes[0].skin.influences[0].bone = 40;
        const Diagnostics report = Validate(document, ValidateLevel::Structural);
        CHECK(errorsOf(report, DiagCode::DanglingNodeReference) == 1u);
    }
    // The values: a duplicate warns, a bad weight fails, disorder is a note.
    {
        Document document = SkinDocument(3, 0, skin, {ProfileId::Wc3Classic});
        CHECK(Validate(document, ValidateLevel::Structural).countOf(DiagCode::SkinWeightInvalid) ==
              0u);
        geom::SkinBinding& binding = document.models[0].meshes[0].skin;
        binding.reset(0);
        binding.appendVertex(std::vector<geom::Influence>{{1, 0.5f}, {1, 0.5f}});
        binding.appendVertex(std::vector<geom::Influence>{{1, -0.5f}});
        binding.appendVertex(std::vector<geom::Influence>{{1, 0.2f}, {2, 0.8f}});
        const Diagnostics report = Validate(document, ValidateLevel::Structural);
        CHECK(report.countOf(DiagCode::SkinInfluenceDuplicated) == 1u);
        CHECK(errorsOf(report, DiagCode::SkinWeightInvalid) == 1u);
        CHECK(report.countOf(DiagCode::SkinInfluencesUnsorted) == 1u);
        CHECK(errorsOf(report, DiagCode::SkinInfluencesUnsorted) == 0u);
        CHECK(errorsOf(report, DiagCode::SkinInfluenceDuplicated) == 0u);

        binding.forVertex(1)[0].weight = std::nanf("");
        CHECK(errorsOf(Validate(document, ValidateLevel::Structural),
                       DiagCode::SkinWeightInvalid) == 1u);
    }
}

// ============================================================================
// S1, the corpus arm
// ============================================================================

TEST_CASE("S1 corpus: every shipped weight comes back where it was",
          "[wem][skin][corpus][.skincorpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 300);
    const MdxConverter converter;

    SkinTally home;  // each document at its own profile
    SkinTally other; // and at the other Warcraft III profile it carries
    u32 read = 0;
    u32 skipped = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            ++skipped;
            continue;
        }
        test::trace(files[i]);
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        mdx::Model source;
        try {
            mdx::Parser parser;
            source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        } catch (...) {
            ++skipped;
            continue;
        }
        const Result<Document> document = converter.fromMdx(source);
        if (!document.ok() || document->models.empty() ||
            document->models.front().meshes.size() != source.geosets.size()) {
            ++skipped;
            continue;
        }
        ++read;
        const std::string file = test::pathText(files[i].filename());
        const ProfileId home_ = document->defaultProfile;
        for (const ProfileId profile : {ProfileId::Wc3Classic, ProfileId::Wc3Reforged}) {
            if (std::find(document->profiles.begin(), document->profiles.end(), profile) ==
                document->profiles.end()) {
                continue;
            }
            const Result<mdx::Model> exported =
                converter.toMdx(*document, profile, VersionFor(profile, source.version));
            if (!exported.ok()) {
                ++(profile == home_ ? home : other).exportsRefused;
                continue;
            }
            const MdxExportMap map = MdxExportMapOf(*document, 0, profile);
            CompareModels(profile == home_ ? home : other, file, source, *exported, map);
        }
    }

    std::cout << "S1 corpus arm: " << read << " files read, " << skipped << " skipped\n";
    // Step 0 of EDIT_MODE_SKIN_PLAN's SL1, the whole corpus through the export
    // before the Skin Quantizer: SKIN 62,406,934 vertices, none differing;
    // 1,069,337 grouped vertices, 844 differing, every one in a group of five
    // to eight bones folded to four; at the other profile 261,634 grouped
    // vertices written as SKIN, 458 differing, the same groups.
    std::cout << "  before SL1 (4,645 files): own profile SKIN 62406934/0, groups 1069337/844 "
                 "(all 5-8 bones); other profile groups 261634/458 (all 5-8 bones)\n";
    home.print("at the document's own profile");
    other.print("at the other profile it carries");
    std::cout << "  repeat groups (" << home.repeatGroups.size() << "):\n";
    for (const std::string& group : home.repeatGroups) {
        std::cout << "    " << group << "\n";
    }

    CHECK(read > 0);
    CHECK(home.requiredSkin() == 0);
    CHECK(home.requiredGroups() == 0);
    CHECK(other.requiredSkin() == 0);
    CHECK(other.requiredGroups() == 0);
}

// ============================================================================
// Found by the review of 2026-09-22
// ============================================================================

TEST_CASE("S1 the quantizer says which vertices it bound to the first bone",
          "[wem][skin][quantize]") {
    // The file names a group for every vertex, so an unbound one's group is
    // invented -- and a Problems view reading the file could not tell.
    const std::vector<std::vector<geom::Influence>> vertices{{}, Rigid(4), {}};
    const skinning::ClassicSkin skin = skinning::QuantizeClassic(vertices, {});
    CHECK(skin.unboundVertices == std::vector<u32>{0, 2});

    SECTION("and one merged away moves on to a group holding that bone") {
        // Measured on no shares at all, every group cost 1/k, so the merge sent
        // the vertex to the group with the MOST bones rather than to its own.
        const std::vector<std::vector<geom::Influence>> merging{
            Blend({{1, 0.6f}, {2, 0.4f}}),                         // group {1, 2}; first bone 1
            {},                                                    // group {1}, used once
            Blend({{6, 0.25f}, {7, 0.25f}, {8, 0.25f}, {9, 0.25f}}),
            Blend({{6, 0.25f}, {7, 0.25f}, {8, 0.25f}, {9, 0.25f}}),
            Blend({{1, 0.6f}, {2, 0.4f}}),
        };
        const skinning::ClassicSkin merged =
            skinning::QuantizeClassic(merging, {}, skinning::ClassicLimits{8, 0.02f, 2});
        REQUIRE(merged.groups.size() == 2u);
        CHECK(merged.mergedGroups == 1u);
        const std::vector<u32>& group = merged.groups[merged.groupOf[1]];
        CHECK(std::find(group.begin(), group.end(), 1u) != group.end());
    }
}

TEST_CASE("S1 a seam's copies count once toward a group's use", "[wem][skin][quantize]") {
    // A uv1 seam splits a vertex in one slicing and not in another; counting
    // each copy made the 256 limit merge different groups in the classic
    // preview than in the classic file.
    const std::vector<std::vector<geom::Influence>> vertices{
        Rigid(1), Rigid(1), // one source vertex, split by a seam
        Rigid(2),
        Rigid(3), Rigid(3), // two real vertices
    };
    const std::vector<u32> sources{10, 10, 11, 12, 13};
    const skinning::ClassicSkin skin =
        skinning::QuantizeClassic(vertices, {}, skinning::ClassicLimits{8, 0.02f, 2}, sources);
    // {1} and {2} are used once each; {3} twice. The earlier of the two goes.
    REQUIRE(skin.groups.size() == 2u);
    CHECK(skin.groups[skin.groupOf[3]] == std::vector<u32>{3});
    CHECK(skin.groups[skin.groupOf[2]] == std::vector<u32>{2});
    CHECK(skin.mergedVertices == std::vector<u32>{0, 1});
}

TEST_CASE("S1 a vertex that binds nothing is written to one bone", "[wem][skin][export]") {
    // `SKIN` had four zero bytes for it, which weigh it by nothing at all; the
    // classic quantizer already bound it to the first bone. Both now do, and
    // both list it.
    const std::vector<std::vector<geom::Influence>> skin = {
        Blend({{2, 0.7f}, {1, 0.3f}}),
        {},
        Rigid(1),
    };
    const MdxConverter converter;
    for (const ProfileId profile : {ProfileId::Wc3Reforged, ProfileId::Wc3Classic}) {
        CAPTURE(static_cast<u32>(profile));
        const Document document = SkinDocument(4, 0, skin, {profile});
        const WrittenSkin written = converter.writtenSkin(document, 0, profile, 0);
        REQUIRE(written.geosets.size() == 1u);
        const WrittenGeosetSkin& geoset = written.geosets[0];
        // The node the first bound vertex, in the geoset's order, leans on most.
        u32 expected = kInvalidNode;
        for (std::size_t v = 0; v < geoset.vertices.size() && expected == kInvalidNode; ++v) {
            if (geoset.vertices[v] != 1) {
                expected = skin[geoset.vertices[v]].front().bone;
            }
        }
        u32 found = 0;
        for (std::size_t v = 0; v < geoset.vertices.size(); ++v) {
            if (geoset.vertices[v] != 1) {
                continue;
            }
            ++found;
            REQUIRE(geoset.influences[v].size() == 1u);
            CHECK(geoset.influences[v][0].node == expected);
            CHECK(std::find(geoset.unbound.begin(), geoset.unbound.end(), v) !=
                  geoset.unbound.end());
        }
        CHECK(found == 1u);
        CHECK(geoset.unbound.size() == 1u);
    }
}
