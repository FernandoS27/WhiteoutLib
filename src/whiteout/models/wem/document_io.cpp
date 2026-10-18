// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/writer.h>

#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/physics/bake.h>

#include "../../common/binary_reader.h"
#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "../../common/unicode_path.h"

#include "binary_read_visitor.h"
#include "binary_write_visitor.h"

#include <algorithm>
#include <fstream>

namespace whiteout {
namespace models {
namespace wem {

namespace {

u32 TagOfNativeKind(NativeKind kind) {
    switch (kind) {
    case NativeKind::Mdx:
        return ChunkTagTraits<native::MdxMaterial>::value;
    case NativeKind::M2:
        return ChunkTagTraits<native::M2Material>::value;
    case NativeKind::M3:
        return ChunkTagTraits<native::M3Material>::value;
    case NativeKind::D3:
        return ChunkTagTraits<native::D3Material>::value;
    case NativeKind::None:
        break;
    }
    return 0;
}

std::vector<u8> ReadWholeFile(const std::string& filePath) {
    auto file = common::open_ifstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
}

} // namespace

// ============================================================================
// IsWemFile
// ============================================================================

bool IsWemFile(std::span<const u8> bytes, u32* versionOut) {
    if (bytes.size() < sizeof(WEMHeader)) {
        return false;
    }
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kWoemMagic) {
        return false;
    }
    if (versionOut != nullptr) {
        *versionOut = header.version;
    }
    return header.version <= kCurrentVersion;
}

// ============================================================================
// Parser
// ============================================================================

namespace {

/// A `.wem` written before levels of detail were dropped kept a World of
/// Warcraft skin profile, and a Diablo III appearance's second geoset array, on
/// `lodLevel`. Neither is a level of detail (EDIT_MODE_MODELLING_DESIGN.md
/// §8.1), so each moves to its section key before the drop could take it. The
/// geometry's game says which: the profile it was authored in.
void KeyLegacySets(Document& document) {
    const bool wow = document.defaultProfile == ProfileId::Wow;
    const bool diablo = document.defaultProfile == ProfileId::Diablo3;
    if (!wow && !diablo) {
        return;
    }
    const char* key = wow ? "m2SkinProfile" : "d3GeoSet";
    const auto legacy = [&](const Mesh& mesh) {
        return wow ? mesh.lodLevel != 0 && mesh.lodLevel != kAllLods : mesh.lodLevel == 1;
    };
    for (Model& model : document.models) {
        if (std::none_of(model.meshes.begin(), model.meshes.end(), legacy)) {
            continue;
        }
        // Every profile keyed, the first included, so the model reads one way.
        for (Mesh& mesh : model.meshes) {
            if (!wow && !legacy(mesh)) {
                continue;
            }
            for (MeshSection& section : mesh.sections) {
                if (section.native.find(key) == nullptr) {
                    section.native.set(key, static_cast<i64>(mesh.lodLevel));
                }
            }
            mesh.lodLevel = 0;
        }
    }
}

/// A `NODE` written before v23 kept an `.m3` bone's scale, and its keys, in
/// the file's axes. WEM's trade X and Y (`m3_converter`'s `RebaseScale`), so
/// each node the `.m3` import made (`m3FlagBits`) is traded here, rest, channel
/// rest and every key.
void TradeLegacyM3Scale(Document& document) {
    const auto trade = [](u8* bytes) {
        Vector3f v;
        std::memcpy(&v, bytes, sizeof(v));
        v = Vector3f{v.y, v.x, v.z};
        std::memcpy(bytes, &v, sizeof(v));
    };
    for (u32 m = 0; m < document.models.size(); ++m) {
        Model& model = document.models[m];
        std::vector<u8> fromM3(model.nodes.size(), 0);
        for (u32 n = 0; n < model.nodes.size(); ++n) {
            Node& node = model.nodes.nodes[n];
            if (node.native.find("m3FlagBits") == nullptr) {
                continue;
            }
            fromM3[n] = 1;
            node.local.scale = Vector3f{node.local.scale.y, node.local.scale.x, node.local.scale.z};
        }
        for (AnimChannel& channel : model.animChannels.channels) {
            const TrackTarget& target = channel.target;
            if (target.kind != TrackTarget::Kind::Node || target.channel != Channel::Scale ||
                channel.valueType != geom::AttrType::F32x3 || target.node >= fromM3.size() || !fromM3[target.node]) {
                continue;
            }
            if (channel.initValue.size() == sizeof(Vector3f)) {
                trade(channel.initValue.data());
            }
            for (Clip& clip : document.clips) {
                if (clip.model != m) {
                    continue;
                }
                for (SubTrackContainer& container : clip.containers) {
                    for (SubTrack& track : container.subTracks) {
                        if (track.channel != channel.id) {
                            continue;
                        }
                        for (std::size_t at = 0; at + sizeof(Vector3f) <= track.values.size(); at += sizeof(Vector3f)) {
                            trade(track.values.data() + at);
                        }
                    }
                }
            }
        }
    }
}

/// A `NODE` written before v25 took an `.m3` light's kind from a `LITE` type
/// read one off: 1, an omni, as a spot, 2, a spot, as a directional, and 0 as
/// an omni. The record beside it keeps the type as the file wrote it, so a
/// kind still saying what that reading made of it is put right; one the
/// document has changed since is left alone.
void RetypeLegacyM3Lights(Document& document) {
    for (Model& model : document.models) {
        for (Node& node : model.nodes.nodes) {
            auto* light = std::get_if<LightPayload>(&node.payload);
            if (light == nullptr || !light->sc2) {
                continue;
            }
            switch (light->sc2->lightType) {
            case native::M3LightType::Omni:
                if (light->kind == LightKind::Spot) {
                    light->kind = LightKind::Omni;
                }
                break;
            case native::M3LightType::Spot:
                if (light->kind == LightKind::Directional) {
                    light->kind = LightKind::Spot;
                }
                break;
            case native::M3LightType::Directional:
                if (light->kind == LightKind::Omni) {
                    light->kind = LightKind::Directional;
                }
                break;
            }
        }
    }
}

/// A `NODE` written before v26 kept a particle system's orientation variant
/// as a link to the ribbon it indexed (`ImportLinks::ribbon`): that ribbon's
/// index among the model's again, or -1 where it named none, which is what the
/// export wrote for it. What that reading had already lost stays lost.
void RestoreOrientationVariants(Document& document) {
    for (Model& model : document.models) {
        std::vector<i32> ribbonIndex(model.nodes.size(), -1);
        i32 ribbons = 0;
        for (u32 n = 0; n < model.nodes.size(); ++n) {
            if (model.nodes.nodes[n].kind == NodeKind::Sc2RibbonEmitter) {
                ribbonIndex[n] = ribbons++;
            }
        }
        for (Node& node : model.nodes.nodes) {
            auto* particle = std::get_if<Sc2ParticleEmitterPayload>(&node.payload);
            if (particle == nullptr) {
                continue;
            }
            if (particle->modelOrientationVariant == Sc2ParticleEmitterPayload::kVariantFromLink) {
                particle->modelOrientationVariant =
                    particle->ribbonLink < ribbonIndex.size() ? ribbonIndex[particle->ribbonLink] : -1;
            }
            particle->ribbonLink = kInvalidNode;
        }
    }
}

/// A `NODE` written before v26 kept a particle system's splat as the file's
/// `PROJ` index: the model's projector of that number, in node order (the
/// order the import made them in), or none.
void RestoreSplatProjectors(Document& document) {
    for (Model& model : document.models) {
        std::vector<u32> projectors;
        for (u32 n = 0; n < model.nodes.size(); ++n) {
            if (model.nodes.nodes[n].kind == NodeKind::Projector) {
                projectors.push_back(n);
            }
        }
        for (Node& node : model.nodes.nodes) {
            auto* particle = std::get_if<Sc2ParticleEmitterPayload>(&node.payload);
            if (particle == nullptr) {
                continue;
            }
            const i32 index = particle->splatProjectionIndex;
            particle->splatProjector = index >= 0 && static_cast<std::size_t>(index) < projectors.size()
                                           ? projectors[static_cast<std::size_t>(index)]
                                           : kInvalidNode;
            particle->splatProjectionIndex = -1;
        }
    }
}

} // namespace

class Parser::Impl {
public:
    Diagnostics diagnostics;
    std::vector<UnknownChunk> unknown;

    std::optional<Document> parse(std::span<const u8> bytes, const ReadOptions& options) {
        diagnostics.clear();
        unknown.clear();

        u32 version = 0;
        if (!IsWemFile(bytes, &version)) {
            diagnostics.error(DiagCode::UnsupportedVersion, "not a WEM file this build can read");
            return std::nullopt;
        }

        if (version != kCurrentVersion) {
            diagnostics.error(DiagCode::UnsupportedVersion,
                              "WEM version " + std::to_string(version) + "; this build reads " +
                                  std::to_string(kCurrentVersion));
            return std::nullopt;
        }

        common::span_streambuf streambuf(bytes);
        std::istream in(&streambuf);
        common::BinaryReader reader(in);

        BinaryReadVisitor visitor(reader);
        for (const NativeKind kind : options.skipNativeKinds) {
            const u32 tag = TagOfNativeKind(kind);
            if (tag != 0) {
                visitor.skipTag(tag);
            }
        }

        Document document;
        visitor.read(document, kCurrentVersion);
        unknown = visitor.unknownChunks();
        // §11.4 rides on the document, not on the parser: an edit-and-write
        // round trip preserves unknown chunks by doing nothing, and dropping
        // them takes clearing this vector.
        document.unknownChunks = unknown;

        for (const std::string& issue : visitor.issues()) {
            diagnostics.warn(DiagCode::ConnectivityCorrupt, issue);
        }
        for (const UnknownChunk& chunk : unknown) {
            diagnostics.info(DiagCode::UnknownChunkPreserved,
                             "preserved chunk at slot " + std::to_string(chunk.index) + " (" +
                                 std::to_string(chunk.data.size()) + " bytes)");
        }

        if (document.models.empty() && !visitor.issues().empty()) {
            return std::nullopt;
        }
        KeyLegacySets(document);
        if (visitor.oldestVersion(ChunkTagTraits<Node>::value) < 23) {
            TradeLegacyM3Scale(document);
        }
        if (visitor.oldestVersion(ChunkTagTraits<Node>::value) < 25) {
            RetypeLegacyM3Lights(document);
        }
        if (visitor.oldestVersion(ChunkTagTraits<Node>::value) < 26) {
            RestoreOrientationVariants(document);
            RestoreSplatProjectors(document);
        }
        DropLevelsOfDetail(document, diagnostics);
        MarkBakeStretches(document);
        return document;
    }
};

Parser::Parser() : impl_(std::make_unique<Impl>()) {}
Parser::~Parser() = default;
Parser::Parser(Parser&&) noexcept = default;
Parser& Parser::operator=(Parser&&) noexcept = default;

std::optional<Document> Parser::parse(const std::string& filePath, const ReadOptions& options) {
    const std::vector<u8> bytes = ReadWholeFile(filePath);
    if (bytes.empty()) {
        impl_->diagnostics.clear();
        impl_->diagnostics.error(DiagCode::UnsupportedVersion, "cannot read " + filePath);
        return std::nullopt;
    }
    return impl_->parse(bytes, options);
}

std::optional<Document> Parser::parse(std::span<const u8> bytes, const ReadOptions& options) {
    return impl_->parse(bytes, options);
}

const std::vector<UnknownChunk>& Parser::unknownChunks() const {
    return impl_->unknown;
}

const Diagnostics& Parser::diagnostics() const {
    return impl_->diagnostics;
}

// ============================================================================
// Writer
// ============================================================================

class Writer::Impl {
public:
    Diagnostics diagnostics;

    std::vector<u8> write(const Document& document) {
        // A mesh an edit renumbered is written in the numbering a fresh build of
        // its face set gives, from a canonicalized copy, compacted of anything
        // lazily deleted (EDIT_MODE_MODELLING_DESIGN.md §2.1). A clean one is
        // written as it is, byte for byte.
        const bool dirty =
            std::any_of(document.models.begin(), document.models.end(), [](const Model& model) {
                return std::any_of(model.meshes.begin(), model.meshes.end(),
                                   [](const Mesh& mesh) { return geom::NumberingDirty(mesh); });
            });
        if (!dirty) {
            return writeAsIs(document);
        }
        Document canonical = document;
        for (Model& model : canonical.models) {
            for (Mesh& mesh : model.meshes) {
                geom::Canonicalize(mesh);
            }
        }
        return writeAsIs(canonical);
    }

private:
    std::vector<u8> writeAsIs(const Document& document) {
        diagnostics.clear();

        std::vector<u8> buffer;
        buffer.reserve(256 * 1024);
        common::vector_streambuf streambuf(buffer);
        std::ostream out(&streambuf);
        common::BinaryWriter writer(out);

        BinaryWriteVisitor visitor(writer);
        visitor.write(document, kCurrentVersion, document.unknownChunks);

        // §11.4's stated limitation, reported rather than hidden: a preserved
        // chunk nothing this build wrote points at is still in the file, but it
        // is reachable only from another preserved chunk.
        for (const u32 slot : visitor.unreferencedUnknownSlots()) {
            diagnostics.warn(DiagCode::OrphanChunk,
                             "preserved chunk at slot " + std::to_string(slot) +
                                 " is not referenced by anything this build wrote");
        }

        buffer.shrink_to_fit();
        return buffer;
    }
};

Writer::Writer() : impl_(std::make_unique<Impl>()) {}
Writer::~Writer() = default;
Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;

std::vector<u8> Writer::write(const Document& document) {
    return impl_->write(document);
}

bool Writer::write(const std::string& filePath, const Document& document) {
    const std::vector<u8> bytes = impl_->write(document);
    auto file = common::open_ofstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

const Diagnostics& Writer::diagnostics() const {
    return impl_->diagnostics;
}

} // namespace wem
} // namespace models
} // namespace whiteout
