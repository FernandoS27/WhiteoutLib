// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/render_view.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

bool isIntegerType(AttrType type) {
    switch (type) {
    case AttrType::U8x4:
    case AttrType::U16:
    case AttrType::U32:
    case AttrType::I32:
    case AttrType::Bool:
        return true;
    default:
        return false;
    }
}

struct ResolvedAttr {
    const AttrLayer* layer = nullptr;
    Domain domain = Domain::Vertex;
    std::size_t stride = 0;
    u32 components = 0;
    bool integer = false;
    utils::AttributeClass attrClass = utils::AttributeClass::Position;
    utils::AttributeEncoding encoding = utils::AttributeEncoding::Float32;
    std::size_t align = 0;
};

f32 readFloat(const AttrLayer& layer, std::size_t element, u32 component) {
    const std::size_t stride = AttrTypeSize(layer.type);
    const std::size_t base = stride * element;
    if (base + stride > layer.data.size()) {
        return 0.0f;
    }
    const u8* bytes = layer.data.data() + base;
    switch (layer.type) {
    case AttrType::F32:
    case AttrType::F32x2:
    case AttrType::F32x3:
    case AttrType::F32x4:
    case AttrType::Quat: {
        f32 value = 0.0f;
        std::memcpy(&value, bytes + sizeof(f32) * component, sizeof(f32));
        return value;
    }
    case AttrType::U8x4:
    case AttrType::Bool:
        return static_cast<f32>(bytes[component]);
    case AttrType::U16: {
        u16 value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<f32>(value);
    }
    case AttrType::U32: {
        u32 value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<f32>(value);
    }
    case AttrType::I32: {
        i32 value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<f32>(value);
    }
    default:
        return 0.0f;
    }
}

u32 readUint(const AttrLayer& layer, std::size_t element, u32 component) {
    const std::size_t stride = AttrTypeSize(layer.type);
    const std::size_t base = stride * element;
    if (base + stride > layer.data.size()) {
        return 0;
    }
    const u8* bytes = layer.data.data() + base;
    switch (layer.type) {
    case AttrType::U8x4:
    case AttrType::Bool:
        return static_cast<u32>(bytes[component]);
    case AttrType::U16: {
        u16 value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<u32>(value);
    }
    case AttrType::U32:
    case AttrType::I32: {
        u32 value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    }
    default:
        return static_cast<u32>(readFloat(layer, element, component));
    }
}

u64 hashBytes(const u8* data, std::size_t size) {
    u64 hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

/// One candidate GPU vertex: a merge group plus one distinct attribute tuple.
struct GroupRec {
    u32 mergeGroup = 0;
    u32 seq = 0; ///< First-seen position in halfedge-within-face order.
    u32 keyOffset = 0;
    u32 keySize = 0;
    u32 vertex = 0;
    u32 halfedge = 0;
};

} // namespace

RenderMeshDesc RenderMeshDesc::Standard() {
    RenderMeshDesc desc;
    desc.attributes.push_back(AttrRequest{names::kPosition, utils::AttributeClass::Position,
                                          utils::AttributeEncoding::Float32, 3, 0});
    desc.attributes.push_back(AttrRequest{names::kNormal, utils::AttributeClass::Normal,
                                          utils::AttributeEncoding::Float32, 3, 0});
    desc.attributes.push_back(AttrRequest{names::uv(0), utils::AttributeClass::UV,
                                          utils::AttributeEncoding::Float32, 2, 0});
    return desc;
}

RenderMesh BuildRenderMesh(const Mesh& mesh, const RenderMeshDesc& desc) {
    RenderMesh out;

    // Work from the cached connectivity when there is one, and from a local copy
    // when there is not — the build is deterministic, so halfedge numbering is
    // the same either way and Halfedge-domain layers index correctly in both.
    Topology local;
    const Topology* topology = nullptr;
    if (mesh.hasConnectivity()) {
        topology = &mesh.topology();
    } else {
        const BuildResult built = local.build(mesh.faceSet());
        if (!built.ok()) {
            out.diagnostics.error(DiagCode::ConnectivityCorrupt,
                                  std::string("render view: ") + ToString(built.error));
            return out;
        }
        topology = &local;
    }

    // --- resolve the requested attributes ------------------------------------

    std::vector<ResolvedAttr> attrs;
    attrs.reserve(desc.attributes.size());
    for (const AttrRequest& request : desc.attributes) {
        const AttrLayer* layer = mesh.attributes.layer(request.name, Domain::Halfedge);
        Domain domain = Domain::Halfedge;
        if (layer == nullptr) {
            layer = mesh.attributes.layer(request.name, Domain::Vertex);
            domain = Domain::Vertex;
        }
        if (layer == nullptr) {
            out.diagnostics.info(DiagCode::Unspecified,
                                 "render view: no layer named '" + request.name + "'");
            continue;
        }
        ResolvedAttr resolved;
        resolved.layer = layer;
        resolved.domain = domain;
        resolved.stride = AttrTypeSize(layer->type);
        resolved.components =
            request.components == 0 ? AttrTypeComponents(layer->type) : request.components;
        resolved.integer = isIntegerType(layer->type);
        resolved.attrClass = request.attrClass;
        resolved.encoding = request.encoding;
        resolved.align = request.align;
        attrs.push_back(resolved);
    }

    const std::span<const u32> mergeGroups =
        mesh.attributes.get<u32>(names::kMergeGroup, Domain::Vertex);
    const std::span<const u32> faceSections = mesh.faceSections();

    // --- group halfedges into GPU vertices ------------------------------------

    std::vector<GroupRec> groups;
    std::vector<u8> keyPool;
    std::unordered_map<u64, std::vector<u32>> byHash; // looked up, never iterated

    std::vector<u32> emitFace; // surviving faces, in mesh order
    std::vector<u32> emitBase; // index into cornerGroup
    std::vector<u32> emitValence;
    std::vector<u32> cornerGroup;

    const u32 faceCount = topology->faceCount();
    emitFace.reserve(faceCount);
    emitBase.reserve(faceCount);
    emitValence.reserve(faceCount);

    std::vector<u8> key;
    u32 sequence = 0;
    for (u32 f = 0; f < faceCount; ++f) {
        const FaceId face(f);
        if (topology->isDeleted(face)) {
            continue;
        }
        emitFace.push_back(f);
        emitBase.push_back(static_cast<u32>(cornerGroup.size()));
        u32 valence = 0;
        for (HalfedgeId h : topology->fh(face)) {
            const VertexId v = topology->from(h);
            const u32 group = v.index() < mergeGroups.size() ? mergeGroups[v.index()] : v.value();

            key.clear();
            const u8* groupBytes = reinterpret_cast<const u8*>(&group);
            key.insert(key.end(), groupBytes, groupBytes + sizeof(group));
            for (const ResolvedAttr& attr : attrs) {
                const std::size_t element = attr.domain == Domain::Halfedge ? h.index() : v.index();
                const std::size_t base = attr.stride * element;
                if (base + attr.stride <= attr.layer->data.size()) {
                    key.insert(key.end(), attr.layer->data.data() + base,
                               attr.layer->data.data() + base + attr.stride);
                } else {
                    key.insert(key.end(), attr.stride, u8{0});
                }
            }

            const u64 hash = hashBytes(key.data(), key.size());
            u32 found = kInvalidId;
            auto bucket = byHash.find(hash);
            if (bucket != byHash.end()) {
                for (u32 candidate : bucket->second) {
                    const GroupRec& record = groups[candidate];
                    if (record.keySize == key.size() &&
                        std::memcmp(keyPool.data() + record.keyOffset, key.data(), key.size()) ==
                            0) {
                        found = candidate;
                        break;
                    }
                }
            }
            if (found == kInvalidId) {
                GroupRec record;
                record.mergeGroup = group;
                record.seq = sequence;
                record.keyOffset = static_cast<u32>(keyPool.size());
                record.keySize = static_cast<u32>(key.size());
                record.vertex = v.value();
                record.halfedge = h.value();
                keyPool.insert(keyPool.end(), key.begin(), key.end());
                found = static_cast<u32>(groups.size());
                groups.push_back(record);
                byHash[hash].push_back(found);
            }
            cornerGroup.push_back(found);
            ++valence;
            ++sequence;
        }
        emitValence.push_back(valence);
    }

    // Emission order is (mergeGroup, first-seen). First-seen alone would be just
    // as deterministic but would not reproduce the *source's* vertex order, and
    // the identity property is a statement about the source's buffers.
    std::vector<u32> order(groups.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        if (groups[a].mergeGroup != groups[b].mergeGroup) {
            return groups[a].mergeGroup < groups[b].mergeGroup;
        }
        return groups[a].seq < groups[b].seq;
    });
    std::vector<u32> gpuOf(groups.size(), 0);
    for (std::size_t i = 0; i < order.size(); ++i) {
        gpuOf[order[i]] = static_cast<u32>(i);
    }

    out.vertexToWemVertex.resize(order.size());
    out.vertexToWemHalfedge.resize(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        out.vertexToWemVertex[i] = groups[order[i]].vertex;
        out.vertexToWemHalfedge[i] = HalfedgeId(groups[order[i]].halfedge);
    }

    // --- index buffer and ranges ----------------------------------------------

    const u32 sectionCount = mesh.sections.empty() ? 1u : static_cast<u32>(mesh.sections.size());
    std::vector<u32> sectionOf(emitFace.size(), 0);
    u32 outOfRange = 0;
    for (std::size_t i = 0; i < emitFace.size(); ++i) {
        const u32 face = emitFace[i];
        u32 section = face < faceSections.size() ? faceSections[face] : 0;
        if (section >= sectionCount) {
            section = 0;
            ++outOfRange;
        }
        sectionOf[i] = section;
    }
    if (outOfRange != 0) {
        out.diagnostics.warn(DiagCode::IndexOutOfRange,
                             std::to_string(outOfRange) +
                                 " face(s) name a section that does not exist; drawn in section 0");
    }

    u32 ngonsFanned = 0;
    const auto emitFaceIndices = [&](std::size_t i) {
        const u32 base = emitBase[i];
        const u32 valence = emitValence[i];
        if (valence < 3) {
            return;
        }
        if (desc.triangulation == TriangulationPolicy::None) {
            for (u32 c = 0; c < valence; ++c) {
                out.indices.push_back(gpuOf[cornerGroup[base + c]]);
            }
            return;
        }
        if (valence > 3) {
            ++ngonsFanned;
        }
        for (u32 c = 1; c + 1 < valence; ++c) {
            out.indices.push_back(gpuOf[cornerGroup[base]]);
            out.indices.push_back(gpuOf[cornerGroup[base + c]]);
            out.indices.push_back(gpuOf[cornerGroup[base + c + 1]]);
        }
    };

    if (desc.splitBySection) {
        for (u32 s = 0; s < sectionCount; ++s) {
            RenderRange range;
            range.section = s;
            range.materialSlot = s < mesh.sections.size() ? mesh.sections[s].materialSlot : 0;
            range.firstIndex = static_cast<u32>(out.indices.size());
            for (std::size_t i = 0; i < emitFace.size(); ++i) {
                if (sectionOf[i] == s) {
                    emitFaceIndices(i);
                }
            }
            range.indexCount = static_cast<u32>(out.indices.size()) - range.firstIndex;
            out.ranges.push_back(range);
        }
    } else {
        for (std::size_t i = 0; i < emitFace.size(); ++i) {
            const u32 section = sectionOf[i];
            if (out.ranges.empty() || out.ranges.back().section != section) {
                RenderRange range;
                range.section = section;
                range.materialSlot =
                    section < mesh.sections.size() ? mesh.sections[section].materialSlot : 0;
                range.firstIndex = static_cast<u32>(out.indices.size());
                out.ranges.push_back(range);
            }
            emitFaceIndices(i);
            out.ranges.back().indexCount =
                static_cast<u32>(out.indices.size()) - out.ranges.back().firstIndex;
        }
    }

    if (ngonsFanned != 0) {
        out.diagnostics.info(DiagCode::NgonTriangulated,
                             std::to_string(ngonsFanned) + " n-gon(s) fanned for the GPU view");
    }

    if (desc.wantU16Indices) {
        if (order.size() <= 0x10000u) {
            out.indices16.reserve(out.indices.size());
            for (u32 index : out.indices) {
                out.indices16.push_back(static_cast<u16>(index));
            }
        } else {
            out.diagnostics.warn(DiagCode::IndexWidthExceeded,
                                 std::to_string(order.size()) +
                                     " GPU vertices do not fit a 16-bit index buffer");
        }
    }

    // --- interleave -----------------------------------------------------------

    utils::VertexBufferBuilder buffer;
    const std::size_t vertexCount = order.size();
    for (const ResolvedAttr& attr : attrs) {
        if (attr.integer) {
            std::vector<u32> flat(vertexCount * attr.components);
            for (std::size_t i = 0; i < vertexCount; ++i) {
                const GroupRec& record = groups[order[i]];
                const std::size_t element =
                    attr.domain == Domain::Halfedge ? record.halfedge : record.vertex;
                for (u32 c = 0; c < attr.components; ++c) {
                    flat[i * attr.components + c] = readUint(*attr.layer, element, c);
                }
            }
            buffer.declareIntAttribute(flat, attr.components, attr.attrClass, attr.encoding,
                                       attr.align);
        } else {
            std::vector<f32> flat(vertexCount * attr.components);
            for (std::size_t i = 0; i < vertexCount; ++i) {
                const GroupRec& record = groups[order[i]];
                const std::size_t element =
                    attr.domain == Domain::Halfedge ? record.halfedge : record.vertex;
                for (u32 c = 0; c < attr.components; ++c) {
                    flat[i * attr.components + c] = readFloat(*attr.layer, element, c);
                }
            }
            buffer.declareFloatAttribute(flat, attr.components, attr.attrClass, attr.encoding,
                                         attr.align);
        }
    }

    if (desc.includeSkin && desc.maxInfluences != 0) {
        const u32 width = desc.maxInfluences;
        std::vector<u32> boneIndices(vertexCount * width, 0);
        std::vector<f32> boneWeights(vertexCount * width, 0.0f);
        const bool fold = !desc.skinParents.empty() && !desc.skinPivots.empty();
        const std::span<const Vector3f> positions =
            mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        u32 overflowed = 0;
        for (std::size_t i = 0; i < vertexCount; ++i) {
            const GroupRec& record = groups[order[i]];

            // A rigid section binds every one of its vertices to one node at
            // weight 1 and never touches the skin array (§5.6).
            const FaceId face = topology->face(HalfedgeId(record.halfedge));
            std::optional<u32> rigid;
            if (face.valid() && face.index() < faceSections.size()) {
                const u32 section = faceSections[face.index()];
                if (section < mesh.sections.size()) {
                    rigid = mesh.sections[section].rigidNode;
                }
            }
            if (rigid.has_value()) {
                boneIndices[i * width] = *rigid;
                boneWeights[i * width] = 1.0f;
                continue;
            }

            std::span<const Influence> influences = mesh.skin.forVertex(record.vertex);
            std::vector<Influence> folded;
            if (influences.size() > width) {
                ++overflowed;
                if (fold && record.vertex < positions.size()) {
                    folded = FoldInfluences(influences, width, positions[record.vertex],
                                            desc.skinParents, desc.skinPivots);
                    influences = folded;
                }
            }
            const std::size_t count = std::min<std::size_t>(influences.size(), width);
            for (std::size_t k = 0; k < count; ++k) {
                boneIndices[i * width + k] = influences[k].bone;
                boneWeights[i * width + k] = influences[k].weight;
            }
        }
        if (overflowed != 0) {
            out.diagnostics.warn(DiagCode::BoneInfluenceLimit,
                                 std::to_string(overflowed) + " vertex/vertices carry more than " +
                                     std::to_string(width) + " influences; " +
                                     (fold ? "the surplus folded into the nearest joints"
                                           : "the tail was cut"));
        }
        buffer.declareIntAttribute(boneIndices, width, utils::AttributeClass::BlendIndices,
                                   desc.blendIndexEncoding);
        buffer.declareFloatAttribute(boneWeights, width, utils::AttributeClass::BlendWeights,
                                     desc.blendWeightEncoding);
    }

    out.vertices = buffer.build();
    return out;
}

std::vector<Influence> FoldInfluences(std::span<const Influence> influences, u32 width,
                                      const Vector3f& position, std::span<const u32> parents,
                                      std::span<const Vector3f> pivots) {
    std::vector<Influence> out(influences.begin(), influences.end());
    // Each influence's chain, the bone first and its root last; the walk stops
    // at a parent past the span, and after as many steps as there are nodes.
    const auto chainOf = [&parents](u32 bone) {
        std::vector<u32> chain;
        for (u32 node = bone; node < parents.size() && chain.size() <= parents.size();
             node = parents[node]) {
            chain.push_back(node);
        }
        return chain;
    };
    const auto lever = [&pivots, &position](u32 node) {
        return node < pivots.size() ? (position - pivots[node]).length() : 0.0f;
    };
    std::vector<std::vector<u32>> chains;
    chains.reserve(out.size());
    for (const Influence& influence : out) {
        chains.push_back(chainOf(influence.bone));
    }

    while (out.size() > width) {
        std::size_t bestFrom = 0;
        std::size_t bestInto = 1;
        f32 bestCost = std::numeric_limits<f32>::max();
        for (std::size_t a = 0; a < out.size(); ++a) {
            for (std::size_t b = a + 1; b < out.size(); ++b) {
                // The joints below the lowest common ancestor on either side;
                // with none in common, both whole chains.
                const std::vector<u32>& ca = chains[a];
                const std::vector<u32>& cb = chains[b];
                std::size_t shared = 0;
                while (shared < ca.size() && shared < cb.size() &&
                       ca[ca.size() - 1 - shared] == cb[cb.size() - 1 - shared]) {
                    ++shared;
                }
                f32 levers = 0.0f;
                for (std::size_t k = 0; k + shared < ca.size(); ++k) {
                    levers += lever(ca[k]);
                }
                for (std::size_t k = 0; k + shared < cb.size(); ++k) {
                    levers += lever(cb[k]);
                }
                // The lighter one moves; on a tie, the one farther from the root.
                const bool aMoves = out[a].weight < out[b].weight ||
                                    (out[a].weight == out[b].weight && ca.size() > cb.size());
                const f32 cost = (aMoves ? out[a].weight : out[b].weight) * levers;
                if (cost < bestCost) {
                    bestCost = cost;
                    bestFrom = aMoves ? a : b;
                    bestInto = aMoves ? b : a;
                }
            }
        }
        out[bestInto].weight += out[bestFrom].weight;
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(bestFrom));
        chains.erase(chains.begin() + static_cast<std::ptrdiff_t>(bestFrom));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Influence& l, const Influence& r) { return l.weight > r.weight; });
    return out;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
