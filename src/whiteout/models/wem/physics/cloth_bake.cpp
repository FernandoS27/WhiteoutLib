// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/cloth_bake.h>

#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/nodes/node.h>
#include <whiteout/models/wem/profile.h>

#include <cstdio>
#include <cstring>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// The eigenvector of @p n's largest eigenvalue (cyclic Jacobi; @p n is
/// symmetric).
std::array<f64, 4> LargestEigenvector(std::array<std::array<f64, 4>, 4> n) {
    std::array<std::array<f64, 4>, 4> v{};
    for (int i = 0; i < 4; ++i) {
        v[i][i] = 1.0;
    }
    for (int sweep = 0; sweep < 32; ++sweep) {
        f64 off = 0.0;
        for (int p = 0; p < 4; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                off += n[p][q] * n[p][q];
            }
        }
        if (off < 1e-24) {
            break;
        }
        for (int p = 0; p < 4; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                if (std::fabs(n[p][q]) < 1e-30) {
                    continue;
                }
                const f64 theta = (n[q][q] - n[p][p]) / (2.0 * n[p][q]);
                const f64 t = (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const f64 c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 4; ++k) {
                    const f64 a = n[k][p], b = n[k][q];
                    n[k][p] = c * a - s * b;
                    n[k][q] = s * a + c * b;
                }
                for (int k = 0; k < 4; ++k) {
                    const f64 a = n[p][k], b = n[q][k];
                    n[p][k] = c * a - s * b;
                    n[q][k] = s * a + c * b;
                }
                for (int k = 0; k < 4; ++k) {
                    const f64 a = v[k][p], b = v[k][q];
                    v[k][p] = c * a - s * b;
                    v[k][q] = s * a + c * b;
                }
            }
        }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i) {
        if (n[i][i] > n[best][best]) {
            best = i;
        }
    }
    return {v[0][best], v[1][best], v[2][best], v[3][best]};
}

u32 DepthOf(const NodeTree& tree, u32 node) {
    u32 depth = 0;
    for (u32 at = tree.nodes[node].parent; at < tree.size() && depth <= tree.size(); at = tree.nodes[at].parent) {
        ++depth;
    }
    return depth;
}

/// A vertex's normalised skin.
std::vector<geom::Influence> SkinOf(const Mesh& mesh, u32 vertex) {
    std::vector<geom::Influence> out;
    if (vertex >= mesh.skin.vertexCount()) {
        return out;
    }
    f32 total = 0.0f;
    for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
        if (influence.weight > 0.0f) {
            out.push_back(influence);
            total += influence.weight;
        }
    }
    for (geom::Influence& influence : out) {
        influence.weight /= total;
    }
    return out;
}

/// Times and values thinned to the keys that keep every dropped sample within
/// @p error of its interpolation (Douglas-Peucker over the samples).
template <class T, class Lerp, class Error>
void Thin(const std::vector<T>& values, f32 tolerance, Lerp lerp, Error error, std::vector<u32>& kept) {
    kept.clear();
    if (values.empty()) {
        return;
    }
    std::vector<u8> keep(values.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<u32, u32>> spans{{0u, static_cast<u32>(values.size() - 1)}};
    while (!spans.empty()) {
        const auto [a, b] = spans.back();
        spans.pop_back();
        f32 worst = 0.0f;
        u32 at = a;
        for (u32 i = a + 1; i < b; ++i) {
            const f32 u = static_cast<f32>(i - a) / static_cast<f32>(b - a);
            const f32 e = error(lerp(values[a], values[b], u), values[i]);
            if (e > worst) {
                worst = e;
                at = i;
            }
        }
        if (worst > tolerance) {
            keep[at] = 1;
            spans.emplace_back(a, at);
            spans.emplace_back(at, b);
        }
    }
    for (u32 i = 0; i < keep.size(); ++i) {
        if (keep[i] != 0) {
            kept.push_back(i);
        }
    }
}

} // namespace

ClothDrivers ClothDriversOf(const Model& model, const Cloth& cloth) {
    ClothDrivers out;
    if (cloth.cage.mesh >= model.meshes.size()) {
        return out;
    }
    const Mesh& mesh = model.meshes[cloth.cage.mesh];
    const auto movable = mesh.attributes.get<u8>(geom::names::kClothMovable, geom::Domain::Vertex);
    const geom::FaceSet& set = mesh.faceSet();
    const std::span<const u32> sections = mesh.faceSections();
    std::set<u32> cage;
    for (std::size_t f = 0, corner = 0; f < set.faceValence.size(); corner += set.faceValence[f], ++f) {
        if (f < sections.size() && sections[f] == cloth.cage.section) {
            for (u32 k = 0; k < set.faceValence[f]; ++k) {
                cage.insert(set.cornerVertex[corner + k]);
            }
        }
    }
    std::set<u32> own;
    if (cloth.recipe) {
        own.insert(cloth.recipe->bones.begin(), cloth.recipe->bones.end());
    }
    std::map<u32, f32> weigh;
    for (const u32 v : cage) {
        const bool free = v < movable.size() && movable[v] != 0;
        if (free) {
            out.vertices.push_back(v);
            continue;
        }
        for (const geom::Influence& influence : SkinOf(mesh, v)) {
            if (own.count(influence.bone) == 0) {
                weigh[influence.bone] += influence.weight;
            }
        }
    }
    f32 most = 0.0f;
    for (const auto& [bone, weight] : weigh) {
        if (weight > most) {
            most = weight;
            out.holder = bone;
        }
    }
    return out;
}

u32 FindClothDriverChannel(const Model& model, u32 cloth, u32 driver, Channel channel) {
    for (const AnimChannel& entry : model.animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Physics && entry.target.channel == channel &&
            entry.target.sub == ClothDriverSub(cloth, driver)) {
            return entry.id;
        }
    }
    return kInvalidIndex;
}

u32 ClothDriverChannel(Model& model, u32 cloth, u32 driver, Channel channel) {
    if (const u32 found = FindClothDriverChannel(model, cloth, driver, channel); found != kInvalidIndex) {
        return found;
    }
    AnimChannel made;
    made.id = model.animChannels.nextFreeId();
    made.target.kind = TrackTarget::Kind::Physics;
    made.target.sub = ClothDriverSub(cloth, driver);
    made.target.channel = channel;
    made.valueType = DefaultValueType(channel);
    return model.animChannels.add(made);
}

Matrix44f AnchoredFrame(const Mesh& mesh, u32 vertex, const Pose& pose) {
    const std::span<const Vector3f> positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    Matrix44f blended;
    for (auto& row : blended.data) {
        for (f32& value : row) {
            value = 0.0f;
        }
    }
    f32 total = 0.0f;
    for (const geom::Influence& influence : SkinOf(mesh, vertex)) {
        if (influence.bone >= pose.skinning.size()) {
            continue;
        }
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                blended.data[r][c] += pose.skinning[influence.bone].data[r][c] * influence.weight;
            }
        }
        total += influence.weight;
    }
    const Vector3f rest = vertex < positions.size() ? positions[vertex] : Vector3f{0, 0, 0};
    if (!(total > 0.0f)) {
        Matrix44f still = Matrix44f::identity();
        still.data[3][0] = rest.x, still.data[3][1] = rest.y, still.data[3][2] = rest.z;
        return still;
    }
    Transform turned = FromMatrix(blended);
    turned.translation = transform_point(rest, blended);
    turned.scale = Vector3f{1, 1, 1};
    return ToMatrix(turned);
}

bool DriverFramesAt(const Document& document, u32 model, const Cloth& cloth, const ClothDrivers& drivers, u32 clip,
                    f32 seconds, const Pose& pose, std::vector<Matrix44f>& frames) {
    if (clip >= document.clips.size() || model >= document.models.size()) {
        return false;
    }
    const Model& owner = document.models[model];
    const Clip& played = document.clips[clip];
    const SampleWindow window = ClipWindow(played, Milliseconds(seconds), -1);
    const Matrix44f holder =
        drivers.holder < pose.frame.size() ? pose.frame[drivers.holder] : Matrix44f::identity();
    frames.assign(drivers.vertices.size(), Matrix44f::identity());
    bool any = false;
    for (u32 d = 0; d < drivers.vertices.size(); ++d) {
        Transform local;
        for (const Channel which : {Channel::ClothDriverTranslation, Channel::ClothDriverRotation}) {
            const u32 channel = FindClothDriverChannel(owner, cloth.id, d, which);
            const SubTrack* track = channel != kInvalidIndex ? FindSubTrack(played, channel) : nullptr;
            if (track == nullptr || track->times.empty()) {
                continue;
            }
            const std::optional<std::vector<u8>> value =
                ReadTrack(played, *track, DefaultValueType(which), window, false);
            if (!value) {
                continue;
            }
            any = true;
            if (which == Channel::ClothDriverTranslation && value->size() >= sizeof(Vector3f)) {
                std::memcpy(&local.translation, value->data(), sizeof(Vector3f));
            } else if (which == Channel::ClothDriverRotation && value->size() >= sizeof(Quaternion)) {
                std::memcpy(&local.rotation, value->data(), sizeof(Quaternion));
            }
        }
        frames[d] = ToMatrix(local) * holder;
    }
    if (!any) {
        frames.clear();
    }
    return any;
}

bool DriverParticlesAt(const Document& document, u32 model, const Cloth& cloth, u32 clip, f32 seconds,
                       const Pose& pose, std::vector<u32>& particleVertex, std::vector<Matrix44f>& frames) {
    particleVertex.clear();
    frames.clear();
    if (model >= document.models.size() || cloth.cage.mesh >= document.models[model].meshes.size()) {
        return false;
    }
    const Model& owner = document.models[model];
    const ClothDrivers drivers = ClothDriversOf(owner, cloth);
    if (drivers.vertices.empty() || !DriverFramesAt(document, model, cloth, drivers, clip, seconds, pose, frames)) {
        return false;
    }
    particleVertex = drivers.vertices;
    // The pinned ones: every other cage vertex a lane names.
    const Mesh& mesh = owner.meshes[cloth.cage.mesh];
    const auto lanes = mesh.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
    std::set<u32> pinned;
    for (const u32 v : ClothDrawnVertices(owner, cloth)) {
        for (u32 k = 0; v < lanes.size() && k < 4; ++k) {
            if (lanes[v][k] != geom::kInvalidId &&
                !std::binary_search(drivers.vertices.begin(), drivers.vertices.end(), lanes[v][k])) {
                pinned.insert(lanes[v][k]);
            }
        }
    }
    for (const u32 v : pinned) {
        particleVertex.push_back(v);
        frames.push_back(AnchoredFrame(mesh, v, pose));
    }
    return true;
}

ClothDrawn SkinClothDrawn(const Model& model, const Cloth& cloth, std::span<const u32> particleVertex,
                          std::span<const Matrix44f> frames) {
    ClothDrawn out;
    out.mesh = cloth.cage.mesh;
    if (cloth.cage.mesh >= model.meshes.size()) {
        return out;
    }
    const Mesh& mesh = model.meshes[cloth.cage.mesh];
    const std::span<const Vector3f> positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const auto lanes = mesh.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
    const auto weights = mesh.attributes.get<std::array<f32, 4>>(geom::names::kClothBindWeight, geom::Domain::Vertex);
    std::map<u32, u32> frameOf;
    for (u32 k = 0; k < particleVertex.size() && k < frames.size(); ++k) {
        frameOf[particleVertex[k]] = k;
    }
    for (const u32 v : ClothDrawnVertices(model, cloth)) {
        if (v >= lanes.size() || v >= weights.size() || v >= positions.size()) {
            continue;
        }
        Vector3f at{0, 0, 0};
        Matrix44f turn;
        for (auto& row : turn.data) {
            for (f32& value : row) {
                value = 0.0f;
            }
        }
        f32 sum = 0.0f;
        for (u32 k = 0; k < 4; ++k) {
            const auto found = lanes[v][k] == geom::kInvalidId ? frameOf.end() : frameOf.find(lanes[v][k]);
            if (found == frameOf.end() || !(weights[v][k] > 0.0f) || lanes[v][k] >= positions.size()) {
                continue;
            }
            const Matrix44f& frame = frames[found->second];
            at = at + transform_point(positions[v] - positions[lanes[v][k]], frame) * weights[v][k];
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    turn.data[r][c] += frame.data[r][c] * weights[v][k];
                }
            }
            sum += weights[v][k];
        }
        if (!(sum > 0.0f)) {
            continue;
        }
        turn.data[3][3] = 1.0f;
        out.vertices.push_back(v);
        out.positions.push_back(at / sum);
        out.rotations.push_back(turn);
    }
    return out;
}

std::vector<u32> ClothDrawnVertices(const Model& model, const Cloth& cloth) {
    std::set<u32> drawn;
    for (const ClothBinding& binding : cloth.bindings) {
        if (binding.section.mesh != cloth.cage.mesh || binding.section.mesh >= model.meshes.size()) {
            continue;
        }
        const Mesh& mesh = model.meshes[binding.section.mesh];
        const geom::FaceSet& set = mesh.faceSet();
        const std::span<const u32> sections = mesh.faceSections();
        for (std::size_t f = 0, corner = 0; f < set.faceValence.size(); corner += set.faceValence[f], ++f) {
            if (f < sections.size() && sections[f] == binding.section.section) {
                for (u32 k = 0; k < set.faceValence[f]; ++k) {
                    drawn.insert(set.cornerVertex[corner + k]);
                }
            }
        }
    }
    return {drawn.begin(), drawn.end()};
}

namespace {

/// A bone named @p name under @p parent, standing at model-space @p rest:
/// its pivot on a pivot rig, its local and every pose entry on an explicit
/// one. Returns its index.
u32 AddBoneAt(NodeTree& tree, std::string name, u32 parent, const Matrix44f& rest) {
    Node node;
    node.name = std::move(name);
    node.kind = NodeKind::Bone;
    node.resetPayloadForKind();
    node.parent = parent;
    const Matrix44f parentRest = parent < tree.size() ? ToMatrix(tree.worldBind(parent)) : Matrix44f::identity();
    if (tree.rig == RigConvention::PivotRelative) {
        // As `fromMdx` states a node: the pivot difference, nothing turned.
        node.pivot = Vector3f{rest.data[3][0], rest.data[3][1], rest.data[3][2]};
        node.local = Transform::identity();
        node.local.translation = node.pivot - (parent < tree.size() ? tree.nodes[parent].pivot : Vector3f{0, 0, 0});
    } else {
        node.local = FromMatrix(rest * Matrix44f::inverse(parentRest));
    }
    const bool matrices = std::any_of(tree.nodes.begin(), tree.nodes.end(),
                                      [](const Node& other) { return !other.poseMatrices.empty(); });
    for (const PoseSchema& entry : tree.poseSchema) {
        Matrix44f value = entry.space == PoseSpace::Model ? rest : rest * Matrix44f::inverse(parentRest);
        if (entry.inverse) {
            value = Matrix44f::inverse(value);
        }
        node.poses.push_back(FromMatrix(value));
        if (matrices) {
            node.poseMatrices.push_back(entry.storage == PoseStorage::Matrix ? value : Matrix44f::identity());
        }
    }
    return tree.add(std::move(node));
}

} // namespace

u32 ExpandClothToBones(Document& document, ProfileId target, Diagnostics& out) {
    const ProfileDesc& profile = Profile(target);
    if (profile.physics.cloth) {
        return 0;
    }
    const u32 limit = std::max<u32>(1, profile.maxBoneInfluences);
    u32 made = 0;
    for (u32 m = 0; m < document.models.size(); ++m) {
        for (u32 c = 0; c < document.models[m].physics.cloths.size(); ++c) {
            const Cloth cloth = document.models[m].physics.cloths[c];
            if (!cloth.recipe || cloth.recipe->bakeInto != ClothBakeInto::FullDetail ||
                cloth.cage.mesh >= document.models[m].meshes.size()) {
                continue;
            }
            const ClothDrivers drivers = ClothDriversOf(document.models[m], cloth);
            if (drivers.vertices.empty()) {
                continue;
            }
            // Every clip's driver frames first, while the tree is the clips'.
            struct Sampled {
                u32 clip = 0;
                f32 dt = 0.0f;
                std::vector<Matrix44f> holder;              ///< Per step.
                std::vector<std::vector<Matrix44f>> frames; ///< Per step, per driver.
            };
            std::vector<Sampled> sampled;
            {
                const Model& owner = document.models[m];
                const Mesh& mesh = owner.meshes[cloth.cage.mesh];
                const Animator animator(document, m);
                for (u32 k = 0; k < document.clips.size(); ++k) {
                    const Clip& clip = document.clips[k];
                    if (clip.model != m || clip.duration <= 0.0f) {
                        continue;
                    }
                    Sampled& s = sampled.emplace_back();
                    s.clip = k;
                    const u32 steps = std::max<u32>(1, static_cast<u32>(std::ceil(clip.duration * 60.0f - 1e-3f)));
                    s.dt = clip.duration / static_cast<f32>(steps);
                    for (u32 i = 0; i <= steps; ++i) {
                        Mix mix;
                        mix.plays.push_back(Play{k, static_cast<f32>(i) * s.dt, 1.0f, false});
                        mix.globals = false;
                        Pose pose;
                        animator.evaluate(mix, pose);
                        std::vector<Matrix44f> frames;
                        if (!DriverFramesAt(document, m, cloth, drivers, k, static_cast<f32>(i) * s.dt, pose, frames)) {
                            for (const u32 v : drivers.vertices) {
                                frames.push_back(AnchoredFrame(mesh, v, pose));
                            }
                        }
                        s.holder.push_back(drivers.holder < pose.frame.size() ? pose.frame[drivers.holder]
                                                                               : Matrix44f::identity());
                        s.frames.push_back(std::move(frames));
                    }
                }
            }

            Model& owner = document.models[m];
            Mesh& mesh = owner.meshes[cloth.cage.mesh];
            const std::span<const Vector3f> positions =
                mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
            // A bone per driver, under the holder, at its particle's rest.
            std::map<u32, u32> boneOf;
            for (u32 d = 0; d < drivers.vertices.size(); ++d) {
                char name[96];
                std::snprintf(name, sizeof name, "%s_%02u", cloth.recipe->name.c_str(), d + 1);
                Matrix44f rest = Matrix44f::identity();
                const Vector3f at = drivers.vertices[d] < positions.size() ? positions[drivers.vertices[d]] : Vector3f{0, 0, 0};
                rest.data[3][0] = at.x, rest.data[3][1] = at.y, rest.data[3][2] = at.z;
                boneOf[drivers.vertices[d]] = AddBoneAt(owner.nodes, name, drivers.holder, rest);
            }
            // The drawn faces on the drivers; a pinned particle's share on its
            // anchors, as it hangs.
            const auto lanes = mesh.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
            const auto weights =
                mesh.attributes.get<std::array<f32, 4>>(geom::names::kClothBindWeight, geom::Domain::Vertex);
            for (const u32 v : ClothDrawnVertices(owner, cloth)) {
                if (v >= lanes.size() || v >= weights.size()) {
                    continue;
                }
                std::map<u32, f32> skin;
                for (u32 k = 0; k < 4; ++k) {
                    if (lanes[v][k] == geom::kInvalidId || !(weights[v][k] > 0.0f)) {
                        continue;
                    }
                    if (const auto driver = boneOf.find(lanes[v][k]); driver != boneOf.end()) {
                        skin[driver->second] += weights[v][k];
                        continue;
                    }
                    for (const geom::Influence& influence : SkinOf(mesh, lanes[v][k])) {
                        skin[influence.bone] += weights[v][k] * influence.weight;
                    }
                }
                std::vector<geom::Influence> kept;
                for (const auto& [bone, weight] : skin) {
                    kept.push_back(geom::Influence{bone, weight});
                }
                std::sort(kept.begin(), kept.end(),
                          [](const geom::Influence& a, const geom::Influence& b) { return a.weight > b.weight; });
                if (kept.size() > limit) {
                    kept.resize(limit);
                }
                f32 total = 0.0f;
                for (const geom::Influence& influence : kept) {
                    total += influence.weight;
                }
                if (!(total > 0.0f)) {
                    continue;
                }
                for (geom::Influence& influence : kept) {
                    influence.weight /= total;
                }
                mesh.skin.assignVertex(v, kept);
            }
            // Every clip keyed, its samples thinned to a thousandth of the
            // cloth's size.
            Vector3f lower{0, 0, 0}, upper{0, 0, 0};
            bool any = false;
            for (const u32 v : ClothDrawnVertices(owner, cloth)) {
                if (v < positions.size()) {
                    const Vector3f& p = positions[v];
                    lower = any ? Vector3f{std::min(lower.x, p.x), std::min(lower.y, p.y), std::min(lower.z, p.z)} : p;
                    upper = any ? Vector3f{std::max(upper.x, p.x), std::max(upper.y, p.y), std::max(upper.z, p.z)} : p;
                    any = true;
                }
            }
            const f32 size = std::max((upper - lower).length(), 1e-3f);
            const f32 tolerance = size * 1e-3f;
            const NodeTree& tree = owner.nodes;
            for (const Sampled& s : sampled) {
                Clip& clip = document.clips[s.clip];
                if (clip.containers.empty()) {
                    clip.containers.emplace_back();
                }
                for (u32 d = 0; d < drivers.vertices.size(); ++d) {
                    const u32 node = boneOf[drivers.vertices[d]];
                    std::vector<Vector3f> moves;
                    std::vector<Quaternion> turns;
                    for (std::size_t i = 0; i < s.frames.size(); ++i) {
                        Pose pose;
                        pose.frame.assign(tree.size(), Matrix44f::identity());
                        pose.local.assign(tree.size(), Transform{});
                        if (drivers.holder < tree.size()) {
                            pose.frame[drivers.holder] = s.holder[i];
                        }
                        const Transform local = StageLocalFor(tree, pose, node, s.frames[i][d]);
                        moves.push_back(local.translation);
                        Quaternion q = local.rotation;
                        if (!turns.empty() && q.dot(turns.back()) < 0.0f) {
                            q = q * -1.0f;
                        }
                        turns.push_back(q);
                    }
                    std::vector<u32> keys;
                    for (const Channel which : {Channel::Translation, Channel::Rotation}) {
                        const bool move = which == Channel::Translation;
                        if (move) {
                            Thin(moves, tolerance, [](const Vector3f& a, const Vector3f& b, f32 u) { return a + (b - a) * u; },
                                 [](const Vector3f& a, const Vector3f& b) { return (a - b).length(); }, keys);
                        } else {
                            Thin(turns, tolerance / size, [](const Quaternion& a, const Quaternion& b, f32 u) {
                                     return Quaternion::slerp(a, b, u);
                                 },
                                 [](const Quaternion& a, const Quaternion& b) {
                                     return 2.0f * std::acos(std::min(1.0f, std::abs(a.dot(b))));
                                 },
                                 keys);
                        }
                        AnimChannel made;
                        made.id = owner.animChannels.nextFreeId();
                        made.target.kind = TrackTarget::Kind::Node;
                        made.target.node = node;
                        made.target.channel = which;
                        made.valueType = move ? geom::AttrType::F32x3 : geom::AttrType::Quat;
                        u32 channel = kInvalidIndex;
                        for (const AnimChannel& entry : owner.animChannels.channels) {
                            if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == node &&
                                entry.target.channel == which) {
                                channel = entry.id;
                            }
                        }
                        if (channel == kInvalidIndex) {
                            channel = owner.animChannels.add(made);
                        }
                        SubTrack track;
                        track.channel = channel;
                        track.interp = move ? Interpolation::Linear : Interpolation::Slerp;
                        for (const u32 i : keys) {
                            track.times.push_back(static_cast<f32>(i) * s.dt);
                            const u8* bytes = move ? reinterpret_cast<const u8*>(&moves[i])
                                                   : reinterpret_cast<const u8*>(&turns[i]);
                            track.values.insert(track.values.end(), bytes,
                                                bytes + (move ? sizeof(Vector3f) : sizeof(Quaternion)));
                        }
                        clip.containers.front().subTracks.push_back(std::move(track));
                    }
                }
            }
            made += static_cast<u32>(drivers.vertices.size());
            out.info(DiagCode::PhysicsUnsupported,
                     cloth.recipe->name + ": " + std::to_string(drivers.vertices.size()) +
                         " bones made from its drivers, the target running no cloth",
                     ElementRef(ElementKind::PhysicsRecord, cloth.id), target);
        }
    }
    return made;
}

bool FitRigid(std::span<const Vector3f> from, std::span<const Vector3f> to, std::span<const f32> weights,
              Transform& out) {
    f64 total = 0.0;
    f64 a[3] = {}, b[3] = {};
    u32 count = 0;
    for (std::size_t i = 0; i < from.size() && i < to.size() && i < weights.size(); ++i) {
        if (!(weights[i] > 0.0f)) {
            continue;
        }
        ++count;
        total += weights[i];
        a[0] += weights[i] * from[i].x, a[1] += weights[i] * from[i].y, a[2] += weights[i] * from[i].z;
        b[0] += weights[i] * to[i].x, b[1] += weights[i] * to[i].y, b[2] += weights[i] * to[i].z;
    }
    if (count < 3 || !(total > 0.0)) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        a[k] /= total;
        b[k] /= total;
    }
    // Horn: the cross-covariance's quaternion form, its largest eigenvector.
    f64 s[3][3] = {};
    for (std::size_t i = 0; i < from.size() && i < to.size() && i < weights.size(); ++i) {
        if (!(weights[i] > 0.0f)) {
            continue;
        }
        const f64 x[3] = {from[i].x - a[0], from[i].y - a[1], from[i].z - a[2]};
        const f64 y[3] = {to[i].x - b[0], to[i].y - b[1], to[i].z - b[2]};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                s[r][c] += weights[i] * x[r] * y[c];
            }
        }
    }
    const std::array<std::array<f64, 4>, 4> n{{
        {s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0]},
        {s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2]},
        {s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1]},
        {s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2]},
    }};
    const std::array<f64, 4> q = LargestEigenvector(n);
    const f64 length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 0.0)) {
        return false;
    }
    out.rotation = Quaternion{static_cast<f32>(q[1] / length), static_cast<f32>(q[2] / length),
                              static_cast<f32>(q[3] / length), static_cast<f32>(q[0] / length)};
    out.scale = Vector3f{1, 1, 1};
    const Vector3f turned = out.rotation.rotate_vector(
        Vector3f{static_cast<f32>(a[0]), static_cast<f32>(a[1]), static_cast<f32>(a[2])});
    out.translation = Vector3f{static_cast<f32>(b[0]) - turned.x, static_cast<f32>(b[1]) - turned.y,
                               static_cast<f32>(b[2]) - turned.z};
    return true;
}

ClothBoneFit::ClothBoneFit(const Document& document, u32 model, const Cloth& cloth,
                           std::span<const u32> particleVertex)
    : document_(&document), model_(model), animator_(document, model), composition_(animator_.composition()) {
    const Model& owner = document.models[model];
    if (!cloth.recipe || cloth.cage.mesh >= owner.meshes.size()) {
        return;
    }
    const Mesh& mesh = owner.meshes[cloth.cage.mesh];
    const std::span<const Vector3f> positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const auto lanes = mesh.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
    const auto weights = mesh.attributes.get<std::array<f32, 4>>(geom::names::kClothBindWeight, geom::Domain::Vertex);
    std::map<u32, u32> particleOf;
    for (u32 k = 0; k < particleVertex.size(); ++k) {
        particleOf[particleVertex[k]] = k;
    }
    const std::set<u32> clothBones(cloth.recipe->bones.begin(), cloth.recipe->bones.end());
    std::set<u32> skinning;
    Vector3f lower{0, 0, 0}, upper{0, 0, 0};
    for (const u32 v : ClothDrawnVertices(owner, cloth)) {
        if (v >= positions.size() || v >= lanes.size() || v >= weights.size() || v >= mesh.skin.vertexCount()) {
            continue;
        }
        std::array<Lane, 4> made{};
        f32 sum = 0.0f;
        for (u32 k = 0; k < 4; ++k) {
            const auto found = lanes[v][k] == geom::kInvalidId ? particleOf.end() : particleOf.find(lanes[v][k]);
            if (found == particleOf.end() || !(weights[v][k] > 0.0f) || lanes[v][k] >= positions.size()) {
                continue;
            }
            made[k] = Lane{found->second, weights[v][k], positions[v] - positions[lanes[v][k]]};
            sum += weights[v][k];
        }
        if (!(sum > 0.0f)) {
            continue;
        }
        std::vector<Influence> influences;
        f32 total = 0.0f;
        for (const geom::Influence& influence : mesh.skin.forVertex(v)) {
            if (influence.weight > 0.0f && influence.bone < owner.nodes.size()) {
                influences.push_back({influence.bone, influence.weight});
                total += influence.weight;
                if (clothBones.count(influence.bone) != 0) {
                    skinning.insert(influence.bone);
                }
            }
        }
        if (!(total > 0.0f)) {
            continue;
        }
        for (Influence& influence : influences) {
            influence.weight /= total;
        }
        lower = drawn_.empty() ? positions[v]
                               : Vector3f{std::min(lower.x, positions[v].x), std::min(lower.y, positions[v].y),
                                          std::min(lower.z, positions[v].z)};
        upper = drawn_.empty() ? positions[v]
                               : Vector3f{std::max(upper.x, positions[v].x), std::max(upper.y, positions[v].y),
                                          std::max(upper.z, positions[v].z)};
        drawn_.push_back(v);
        rest_.push_back(positions[v]);
        lanes_.push_back(made);
        skin_.push_back(std::move(influences));
    }
    size_ = (upper - lower).length();
    bones_.assign(skinning.begin(), skinning.end());
    const NodeTree& tree = owner.nodes;
    std::stable_sort(bones_.begin(), bones_.end(), [&](u32 a, u32 b) { return DepthOf(tree, a) < DepthOf(tree, b); });
}

f32 ClothBoneFit::fit(std::span<const ClothParticleFrame> frames, Pose& pose) const {
    const NodeTree& tree = document_->models[model_].nodes;
    if (!fits() || pose.frame.size() < tree.size() || pose.skinning.size() < tree.size() ||
        pose.local.size() < tree.size()) {
        return 0.0f;
    }
    // Where the cloth puts each drawn point: StarCraft II's formula.
    std::vector<Matrix44f> placed(frames.size());
    for (std::size_t k = 0; k < frames.size(); ++k) {
        placed[k] = ToMatrix(Transform{frames[k].position, frames[k].rotation, Vector3f{1, 1, 1}});
    }
    std::vector<Vector3f> target(drawn_.size(), Vector3f{0, 0, 0});
    std::vector<u8> reached(drawn_.size(), 0);
    for (std::size_t v = 0; v < drawn_.size(); ++v) {
        f32 sum = 0.0f;
        for (const Lane& lane : lanes_[v]) {
            if (lane.weight > 0.0f && lane.particle < placed.size()) {
                target[v] = target[v] + transform_point(lane.offset, placed[lane.particle]) * lane.weight;
                sum += lane.weight;
            }
        }
        if (sum > 0.0f) {
            target[v] = target[v] / sum;
            reached[v] = 1;
        }
    }
    const auto skinned = [&](std::size_t v, u32 skip) {
        Vector3f at{0, 0, 0};
        for (const Influence& influence : skin_[v]) {
            if (influence.bone != skip) {
                at = at + transform_point(rest_[v], pose.skinning[influence.bone]) * influence.weight;
            }
        }
        return at;
    };
    // Each node after @p from in the composition composed again: what moving
    // it carries.
    const auto recompose = [&](u32 from) {
        bool after = false;
        for (std::size_t i = 0; i < composition_.order.size(); ++i) {
            const u32 node = composition_.order[i];
            after = after || node == from;
            if (after && node < tree.size()) {
                animator_.composeNode(pose, node, composition_.parent[node]);
            }
        }
    };

    std::vector<Vector3f> from, to;
    std::vector<f32> weight;
    for (int sweep = 0; sweep < 2; ++sweep) {
        for (const u32 bone : bones_) {
            from.clear();
            to.clear();
            weight.clear();
            f32 total = 0.0f;
            for (std::size_t v = 0; v < drawn_.size(); ++v) {
                f32 w = 0.0f;
                for (const Influence& influence : skin_[v]) {
                    w += influence.bone == bone ? influence.weight : 0.0f;
                }
                if (!reached[v] || w < 1e-3f) {
                    continue;
                }
                // Its share: the target less what the other bones carry.
                const Vector3f rest = target[v] - skinned(v, bone);
                from.push_back(rest_[v]);
                to.push_back(rest / w);
                weight.push_back(w * w);
                total += w * w;
            }
            // A light pull toward where the bone stands, so a ribbon of
            // points in a line cannot spin about itself.
            const Matrix44f& now = pose.skinning[bone];
            const Vector3f centre = from.empty() ? Vector3f{0, 0, 0} : from.front();
            for (const Vector3f axis : {Vector3f{1, 0, 0}, Vector3f{0, 1, 0}, Vector3f{0, 0, 1}}) {
                from.push_back(centre + axis);
                to.push_back(transform_point(centre + axis, now));
                weight.push_back(total * 1e-3f);
            }
            Transform rigid;
            if (total <= 0.0f || !FitRigid(from, to, weight, rigid)) {
                continue;
            }
            // The frame that skins as the fit does: its rest under it.
            const Matrix44f restFrame = pose.frame[bone] * Matrix44f::inverse(now);
            const Matrix44f frame = restFrame * ToMatrix(rigid);
            const Transform local = StageLocalFor(tree, pose, bone, frame);
            pose.local[bone].translation = local.translation;
            pose.local[bone].rotation = local.rotation;
            recompose(bone);
        }
    }
    f32 worst = 0.0f;
    for (std::size_t v = 0; v < drawn_.size(); ++v) {
        if (reached[v]) {
            worst = std::max(worst, (skinned(v, kInvalidNode) - target[v]).length());
        }
    }
    return worst;
}

} // namespace wem
} // namespace models
} // namespace whiteout
