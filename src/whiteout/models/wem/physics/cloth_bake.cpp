// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/cloth_bake.h>

#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/nodes/node.h>
#include <whiteout/models/wem/profile.h>
#include <whiteout/models/wem/skinning/fit.h>

#include "../geometry/fracture/parallel.h"

#include <cstdio>
#include <cstring>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace whiteout {
namespace models {
namespace wem {

namespace {

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

/// What a cloth's made bones are named after: its recipe's name, else the
/// section its faces draw in (an imported cloth's), else its id.
std::string ClothBoneStem(const Model& model, const Cloth& cloth) {
    if (cloth.recipe && !cloth.recipe->name.empty()) {
        return cloth.recipe->name;
    }
    const SectionRef at = cloth.bindings.empty() ? cloth.cage : cloth.bindings.front().section;
    if (at.mesh < model.meshes.size() && at.section < model.meshes[at.mesh].sections.size() &&
        !model.meshes[at.mesh].sections[at.section].name.empty()) {
        return model.meshes[at.mesh].sections[at.section].name;
    }
    return "Cloth" + std::to_string(cloth.id);
}

/// Whether a clip of @p model keys @p cloth's drivers: a bake wrote them.
bool DriversKeyed(const Document& document, u32 model, const Cloth& cloth) {
    const u32 channel = FindClothDriverChannel(document.models[model], cloth.id, 0, Channel::ClothDriverTranslation);
    if (channel == kInvalidIndex) {
        return false;
    }
    return std::any_of(document.clips.begin(), document.clips.end(), [&](const Clip& clip) {
        return clip.model == model && std::any_of(clip.containers.begin(), clip.containers.end(),
                                                  [&](const SubTrackContainer& k) { return k.find(channel) != nullptr; });
    });
}

constexpr Channel kDriverChannels[2] = {Channel::ClothDriverTranslation, Channel::ClothDriverRotation};

/// Each of @p cloth's first @p count drivers' keyed tracks in @p clip,
/// translation then rotation, as `FindClothDriverChannel` and `FindSubTrack`
/// find them; null where none. One pass over the table and the clip, not one
/// a driver: a large cloth has thousands.
std::vector<std::array<const SubTrack*, 2>> DriverTracks(const Model& owner, const Cloth& cloth, u32 count,
                                                         const Clip& clip) {
    std::vector<std::array<u32, 2>> channels(count, {kInvalidIndex, kInvalidIndex});
    for (const AnimChannel& entry : owner.animChannels.channels) {
        const u32 d = DriverOfSub(entry.target.sub);
        for (u32 which = 0; which < 2; ++which) {
            if (entry.target.kind == TrackTarget::Kind::Physics && entry.target.channel == kDriverChannels[which] &&
                d < count && entry.target.sub == ClothDriverSub(cloth.id, d) && channels[d][which] == kInvalidIndex) {
                channels[d][which] = entry.id;
            }
        }
    }
    // A container's first track of a channel, from the first container whose
    // first has keys.
    std::unordered_map<u32, const SubTrack*> keyed;
    std::unordered_set<u32> seen;
    for (const SubTrackContainer& container : clip.containers) {
        seen.clear();
        for (const SubTrack& track : container.subTracks) {
            if (seen.insert(track.channel).second && !track.times.empty()) {
                keyed.try_emplace(track.channel, &track);
            }
        }
    }
    std::vector<std::array<const SubTrack*, 2>> out(count, {nullptr, nullptr});
    for (u32 d = 0; d < count; ++d) {
        for (u32 which = 0; which < 2; ++which) {
            const auto found = keyed.find(channels[d][which]);
            out[d][which] = channels[d][which] != kInvalidIndex && found != keyed.end() ? found->second : nullptr;
        }
    }
    return out;
}

/// `DriverFramesAt` at each of @p timesMs, the holder at @p holders' frame
/// there, each track's keys prepared once rather than at every read. A step
/// no track reads at is left empty.
std::vector<std::vector<Matrix44f>> DriverFramesOver(const Document& document, u32 model, const Cloth& cloth,
                                                     const ClothDrivers& drivers, u32 clip,
                                                     std::span<const i32> timesMs, std::span<const Matrix44f> holders) {
    const Clip& played = document.clips[clip];
    const u32 count = static_cast<u32>(drivers.vertices.size());
    const std::vector<std::array<const SubTrack*, 2>> tracks = DriverTracks(document.models[model], cloth, count, played);
    std::vector<std::vector<Transform>> locals(timesMs.size(), std::vector<Transform>(count));
    std::vector<u8> read(timesMs.size(), 0);
    for (u32 d = 0; d < count; ++d) {
        for (u32 which = 0; which < 2; ++which) {
            if (tracks[d][which] == nullptr) {
                continue;
            }
            const geom::AttrType type = DefaultValueType(kDriverChannels[which]);
            const std::size_t size = geom::AttrTypeSize(type);
            // NaN where the rule reads no key, which `ReadTrack` says as no value.
            const std::vector<u8> nothing(size, 0xFF);
            const std::vector<u8> values = SampleSubTrackBatch(played, *tracks[d][which], type, timesMs, nothing);
            for (std::size_t i = 0; i < timesMs.size() && (i + 1) * size <= values.size(); ++i) {
                const u8* at = values.data() + i * size;
                f32 first = 0.0f;
                std::memcpy(&first, at, sizeof(f32));
                if (std::isnan(first)) {
                    continue;
                }
                read[i] = 1;
                if (which == 0 && size >= sizeof(Vector3f)) {
                    std::memcpy(&locals[i][d].translation, at, sizeof(Vector3f));
                } else if (which == 1 && size >= sizeof(Quaternion)) {
                    std::memcpy(&locals[i][d].rotation, at, sizeof(Quaternion));
                }
            }
        }
    }
    std::vector<std::vector<Matrix44f>> frames(timesMs.size());
    for (std::size_t i = 0; i < timesMs.size() && i < holders.size(); ++i) {
        if (read[i] == 0) {
            continue;
        }
        frames[i].resize(count);
        for (u32 d = 0; d < count; ++d) {
            frames[i][d] = ToMatrix(locals[i][d]) * holders[i];
        }
    }
    return frames;
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
    const std::vector<std::array<const SubTrack*, 2>> tracks =
        DriverTracks(owner, cloth, static_cast<u32>(drivers.vertices.size()), played);
    bool any = false;
    for (u32 d = 0; d < drivers.vertices.size(); ++d) {
        Transform local;
        for (u32 slot = 0; slot < 2; ++slot) {
            const Channel which = kDriverChannels[slot];
            const SubTrack* track = tracks[d][slot];
            if (track == nullptr) {
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

u32 ExpandClothToBones(Document& document, ProfileId target, Diagnostics& out, const ClothBoneOptions& options) {
    const ProfileDesc& profile = Profile(target);
    if (profile.physics.cloth) {
        return 0;
    }
    const u32 limit = std::max<u32>(1, profile.maxBoneInfluences);
    // Sampling allocates as it goes: past the cores the threads wait on the
    // heap (`BakeForExport`).
    const u32 threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    // Each tier's RMS distance, of the cloth's size, and its most bones
    // (BONE_REDUCTION_RESEARCH.md §7.4), Low to Ultra.
    struct Tier {
        f32 rms;
        u32 budget;
    };
    static constexpr Tier kTiers[] = {{0.005f, 24}, {0.0025f, 40}, {0.001f, 64}, {0.0005f, 128}};
    // Past the tiers (Full, or a number a script sent) is a bone per particle.
    const u32 detail = static_cast<u32>(options.detail);
    const Tier* tier = detail < std::size(kTiers) ? &kTiers[detail] : nullptr;
    u32 made = 0;
    for (u32 m = 0; m < document.models.size(); ++m) {
        for (u32 c = 0; c < document.models[m].physics.cloths.size(); ++c) {
            const Cloth cloth = document.models[m].physics.cloths[c];
            if (!BakesFullDetail(cloth) || cloth.cage.mesh >= document.models[m].meshes.size()) {
                continue;
            }
            // An imported cloth no bake keyed goes as its faces' own skin, as it
            // did before the export could bake it.
            if (!cloth.recipe && !DriversKeyed(document, m, cloth)) {
                continue;
            }
            const ClothDrivers drivers = ClothDriversOf(document.models[m], cloth);
            if (drivers.vertices.empty()) {
                continue;
            }
            // A tier fits fewer bones than there are particles, where there are
            // more than it allows.
            bool fits = tier != nullptr && drivers.vertices.size() > tier->budget;
            // The skeleton bones the drawn faces also hang from: the holder, and
            // the anchors of the particles pinned to it.
            std::vector<u32> anchors;
            if (fits) {
                const Mesh& mesh = document.models[m].meshes[cloth.cage.mesh];
                const auto lanes =
                    mesh.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
                std::set<u32> held;
                if (drivers.holder < document.models[m].nodes.size()) {
                    held.insert(drivers.holder);
                }
                for (const u32 v : ClothDrawnVertices(document.models[m], cloth)) {
                    for (u32 k = 0; v < lanes.size() && k < 4; ++k) {
                        if (lanes[v][k] != geom::kInvalidId &&
                            !std::binary_search(drivers.vertices.begin(), drivers.vertices.end(), lanes[v][k])) {
                            for (const geom::Influence& influence : SkinOf(mesh, lanes[v][k])) {
                                held.insert(influence.bone);
                            }
                        }
                    }
                }
                anchors.assign(held.begin(), held.end());
            }
            // Every clip's driver frames first, while the tree is the clips'.
            struct Sampled {
                u32 clip = 0;
                f32 dt = 0.0f;
                std::vector<Matrix44f> holder;              ///< Per step.
                std::vector<std::vector<Matrix44f>> frames; ///< Per step, per bone made.
                std::vector<Matrix44f> anchored;            ///< Per step, per anchor: its skinning.
            };
            std::vector<Sampled> sampled;
            for (u32 k = 0; k < document.clips.size(); ++k) {
                if (document.clips[k].model == m && document.clips[k].duration > 0.0f) {
                    sampled.emplace_back().clip = k;
                }
            }
            {
                const Model& owner = document.models[m];
                const Mesh& mesh = owner.meshes[cloth.cage.mesh];
                // What the threads read and none writes, found before them.
                owner.nodes.ensureHierarchy();
                const Animator::Rests rests = Animator(document, m).resolveRests();
                // Clip by clip, side by side, each its own animator: the bones
                // alone, each track prepared once.
                geom::fracture::Parallel(sampled.size(), threads, [&](std::size_t job) {
                    Sampled& s = sampled[job];
                    const u32 k = s.clip;
                    const Clip& clip = document.clips[k];
                    Animator animator(document, m, rests);
                    animator.keepTracksPrepared();
                    const auto poseAt = [&](f32 seconds) {
                        Mix mix;
                        mix.plays.push_back(Play{k, seconds, 1.0f, false});
                        mix.globals = false;
                        Pose pose;
                        animator.sample(mix, pose, true);
                        animator.compose(pose);
                        return pose;
                    };
                    const u32 steps = std::max<u32>(1, static_cast<u32>(std::ceil(clip.duration * 30.0f - 1e-3f)));
                    s.dt = clip.duration / static_cast<f32>(steps);
                    std::vector<i32> times;
                    for (u32 i = 0; i <= steps; ++i) {
                        const Pose pose = poseAt(static_cast<f32>(i) * s.dt);
                        s.holder.push_back(drivers.holder < pose.frame.size() ? pose.frame[drivers.holder]
                                                                               : Matrix44f::identity());
                        for (const u32 anchor : anchors) {
                            s.anchored.push_back(anchor < pose.skinning.size() ? pose.skinning[anchor]
                                                                               : Matrix44f::identity());
                        }
                        times.push_back(static_cast<i32>(Milliseconds(static_cast<f32>(i) * s.dt)));
                    }
                    s.frames = DriverFramesOver(document, m, cloth, drivers, k, times, s.holder);
                    for (u32 i = 0; i <= steps; ++i) {
                        if (s.frames[i].empty()) {
                            const Pose pose = poseAt(static_cast<f32>(i) * s.dt);
                            for (const u32 v : drivers.vertices) {
                                s.frames[i].push_back(AnchoredFrame(mesh, v, pose));
                            }
                        }
                    }
                });
            }

            Model& owner = document.models[m];
            Mesh& mesh = owner.meshes[cloth.cage.mesh];
            const std::span<const Vector3f> positions =
                mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
            const auto lanes = mesh.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
            const auto weights =
                mesh.attributes.get<std::array<f32, 4>>(geom::names::kClothBindWeight, geom::Domain::Vertex);
            const std::vector<u32> drawn = ClothDrawnVertices(owner, cloth);
            const std::string stem = ClothBoneStem(owner, cloth);
            std::map<u32, u32> driverOf;
            for (u32 d = 0; d < drivers.vertices.size(); ++d) {
                driverOf[drivers.vertices[d]] = d;
            }
            // Each bone's samples are thinned to what moves a vertex it carries
            // by ε: a thousandth of the cloth's size, or the export's
            // reduction's where that is more. A turn moves a vertex by its
            // reach, and tilts the faces at the bone: ε over their edge is the
            // tilt a move of ε gives them.
            Vector3f lower{0, 0, 0}, upper{0, 0, 0};
            bool any = false;
            for (const u32 v : drawn) {
                if (v < positions.size()) {
                    const Vector3f& p = positions[v];
                    lower = any ? Vector3f{std::min(lower.x, p.x), std::min(lower.y, p.y), std::min(lower.z, p.z)} : p;
                    upper = any ? Vector3f{std::max(upper.x, p.x), std::max(upper.y, p.y), std::max(upper.z, p.z)} : p;
                    any = true;
                }
            }
            const f32 size = std::max((upper - lower).length(), 1e-3f);
            f32 tolerance = size * 1e-3f;
            if (options.reduce != nullptr) {
                tolerance = std::max(tolerance, options.reduce->tolerance > 0.0f
                                                    ? options.reduce->tolerance
                                                    : options.reduce->heightShare * ModelHeight(owner));
            }
            // The cage's edge at each particle, and over the cloth.
            std::vector<f32> edge(drivers.vertices.size(), size);
            f32 meanEdge = size;
            {
                std::vector<f32> sum(drivers.vertices.size(), 0.0f);
                std::vector<u32> edges(drivers.vertices.size(), 0);
                f64 all = 0.0;
                u32 counted = 0;
                const geom::FaceSet& set = std::as_const(mesh).faceSet();
                const std::span<const u32> sections = std::as_const(mesh).faceSections();
                for (std::size_t f = 0, corner = 0; f < set.faceValence.size(); corner += set.faceValence[f], ++f) {
                    for (u32 k = 0; f < sections.size() && sections[f] == cloth.cage.section && k < set.faceValence[f];
                         ++k) {
                        const u32 a = set.cornerVertex[corner + k];
                        const u32 b = set.cornerVertex[corner + (k + 1) % set.faceValence[f]];
                        if (a >= positions.size() || b >= positions.size()) {
                            continue;
                        }
                        const f32 length = (positions[a] - positions[b]).length();
                        all += length;
                        ++counted;
                        for (const u32 end : {a, b}) {
                            if (const auto driver = driverOf.find(end); driver != driverOf.end()) {
                                sum[driver->second] += length;
                                ++edges[driver->second];
                            }
                        }
                    }
                }
                for (u32 d = 0; d < drivers.vertices.size(); ++d) {
                    edge[d] = edges[d] != 0 ? sum[d] / static_cast<f32>(edges[d]) : size;
                }
                meanEdge = counted != 0 ? static_cast<f32>(all / counted) : size;
            }

            // The bones: a tier's, fitted to where the drawn faces go, or one
            // per driver. Either hangs under the holder, at rest where it
            // stands, and `frames` becomes theirs.
            std::vector<u32> bones;
            std::vector<f32> lever;
            std::set<u32> fitted;
            f32 fitRms = 0.0f;
            if (fits) {
                // The faces as a bone per driver would draw them, every step of
                // every clip: what the fewer bones reproduce.
                std::vector<u32> points;
                for (const u32 v : drawn) {
                    for (u32 k = 0; v < lanes.size() && v < weights.size() && k < 4; ++k) {
                        if (weights[v][k] > 0.0f && driverOf.count(lanes[v][k]) != 0 && v < positions.size()) {
                            points.push_back(v);
                            break;
                        }
                    }
                }
                std::map<u32, u32> anchorOf;
                for (u32 a = 0; a < anchors.size(); ++a) {
                    anchorOf[anchors[a]] = a;
                }
                u32 frames = 0;
                for (const Sampled& s : sampled) {
                    frames += static_cast<u32>(s.frames.size());
                }
                const std::size_t count = points.size(), held = anchors.size();
                std::vector<Vector3f> rest(count), motion(static_cast<std::size_t>(frames) * count);
                std::vector<Matrix44f> fixedMotion(static_cast<std::size_t>(frames) * held);
                std::size_t f = 0;
                for (const Sampled& s : sampled) {
                    for (std::size_t i = 0; i < s.frames.size(); ++i, ++f) {
                        for (std::size_t a = 0; a < held; ++a) {
                            fixedMotion[f * held + a] = s.anchored[i * held + a];
                        }
                        for (std::size_t p = 0; p < count; ++p) {
                            const u32 v = points[p];
                            Vector3f at{0, 0, 0};
                            f32 sum = 0.0f;
                            for (u32 k = 0; k < 4; ++k) {
                                const u32 lane = lanes[v][k];
                                if (lane == geom::kInvalidId || !(weights[v][k] > 0.0f) || lane >= positions.size()) {
                                    continue;
                                }
                                if (const auto driver = driverOf.find(lane); driver != driverOf.end()) {
                                    at = at + transform_point(positions[v] - positions[lane],
                                                              s.frames[i][driver->second]) *
                                                  weights[v][k];
                                } else {
                                    for (const geom::Influence& influence : SkinOf(mesh, lane)) {
                                        at = at + transform_point(positions[v],
                                                                  fixedMotion[f * held + anchorOf[influence.bone]]) *
                                                      (weights[v][k] * influence.weight);
                                    }
                                }
                                sum += weights[v][k];
                            }
                            motion[f * count + p] = at / sum;
                        }
                    }
                }
                std::vector<std::vector<u32>> allowed(count);
                for (std::size_t p = 0; p < count; ++p) {
                    rest[p] = positions[points[p]];
                    std::set<u32> own;
                    if (const auto found = anchorOf.find(drivers.holder); found != anchorOf.end()) {
                        own.insert(found->second);
                    }
                    for (u32 k = 0; k < 4; ++k) {
                        const u32 lane = lanes[points[p]][k];
                        if (lane != geom::kInvalidId && weights[points[p]][k] > 0.0f && driverOf.count(lane) == 0) {
                            for (const geom::Influence& influence : SkinOf(mesh, lane)) {
                                own.insert(anchorOf[influence.bone]);
                            }
                        }
                    }
                    allowed[p].assign(own.begin(), own.end());
                }
                skinning::FitOptions fit;
                fit.maxBones = tier->budget;
                fit.rmsTarget = tier->rms * size;
                fit.maxInfluences = std::min<u32>(limit, 4);
                // Rounded as the target's file will hold them (`SoftWeightsFor`'s rule).
                fit.weights = target == ProfileId::Wc3Classic ? skinning::FitWeights::EqualShares
                              : target == ProfileId::Generic  ? skinning::FitWeights::Exact
                                                              : skinning::FitWeights::Bytes;
                const skinning::FitResult result =
                    skinning::FitBones(skinning::FitMotion{rest, motion, frames},
                                       skinning::FitFixedBones{fixedMotion, static_cast<u32>(held), allowed}, fit);
                fits = result.bones > 0;
                if (fits) {
                    fitRms = result.rms;
                    const auto restOf = [&](u32 k) {
                        Matrix44f at = Matrix44f::identity();
                        at.data[3][0] = result.pivots[k].x, at.data[3][1] = result.pivots[k].y,
                        at.data[3][2] = result.pivots[k].z;
                        return at;
                    };
                    for (u32 k = 0; k < result.bones; ++k) {
                        char name[96];
                        std::snprintf(name, sizeof name, "%s_%02u", stem.c_str(), k + 1);
                        bones.push_back(AddBoneAt(owner.nodes, name, drivers.holder, restOf(k)));
                        lever.push_back(meanEdge);
                    }
                    for (std::size_t p = 0; p < count; ++p) {
                        std::vector<geom::Influence> kept;
                        for (const geom::Influence& influence : result.weights[p]) {
                            const bool own = influence.bone < result.bones;
                            kept.push_back(geom::Influence{
                                own ? bones[influence.bone] : anchors[influence.bone - result.bones], influence.weight});
                            if (own) {
                                lever[influence.bone] =
                                    std::max(lever[influence.bone], (rest[p] - result.pivots[influence.bone]).length());
                            }
                        }
                        mesh.skin.assignVertex(points[p], kept);
                        fitted.insert(points[p]);
                    }
                    // Each step's frames are the fitted bones' from here.
                    f = 0;
                    for (Sampled& s : sampled) {
                        for (std::size_t i = 0; i < s.frames.size(); ++i, ++f) {
                            s.frames[i].resize(result.bones);
                            for (u32 k = 0; k < result.bones; ++k) {
                                s.frames[i][k] = restOf(k) * result.skinning[f * result.bones + k];
                            }
                        }
                    }
                }
            }
            std::map<u32, u32> boneOf;
            if (!fits) {
                for (u32 d = 0; d < drivers.vertices.size(); ++d) {
                    char name[96];
                    std::snprintf(name, sizeof name, "%s_%02u", stem.c_str(), d + 1);
                    Matrix44f rest = Matrix44f::identity();
                    const Vector3f at =
                        drivers.vertices[d] < positions.size() ? positions[drivers.vertices[d]] : Vector3f{0, 0, 0};
                    rest.data[3][0] = at.x, rest.data[3][1] = at.y, rest.data[3][2] = at.z;
                    bones.push_back(AddBoneAt(owner.nodes, name, drivers.holder, rest));
                    boneOf[drivers.vertices[d]] = bones.back();
                    lever.push_back(edge[d]);
                }
            }
            // The other drawn faces on the drivers' bones; a pinned particle's
            // share on its anchors, as it hangs.
            for (const u32 v : drawn) {
                if (v >= lanes.size() || v >= weights.size() || fitted.count(v) != 0) {
                    continue;
                }
                std::map<u32, f32> skin;
                for (u32 k = 0; k < 4; ++k) {
                    if (lanes[v][k] == geom::kInvalidId || !(weights[v][k] > 0.0f)) {
                        continue;
                    }
                    if (const auto driver = boneOf.find(lanes[v][k]); driver != boneOf.end()) {
                        skin[driver->second] += weights[v][k];
                        if (v < positions.size() && lanes[v][k] < positions.size()) {
                            f32& reach = lever[driverOf[lanes[v][k]]];
                            reach = std::max(reach, (positions[v] - positions[lanes[v][k]]).length());
                        }
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
            const NodeTree& tree = owner.nodes;
            // Each bone and its two channels, declared once for every clip.
            std::vector<std::array<u32, 2>> boneChannels(bones.size(), {kInvalidIndex, kInvalidIndex});
            for (u32 b = 0; b < bones.size() && !sampled.empty(); ++b) {
                for (u32 slot = 0; slot < 2; ++slot) {
                    const Channel which = slot == 0 ? Channel::Translation : Channel::Rotation;
                    for (const AnimChannel& entry : owner.animChannels.channels) {
                        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == bones[b] &&
                            entry.target.channel == which) {
                            boneChannels[b][slot] = entry.id;
                        }
                    }
                    if (boneChannels[b][slot] == kInvalidIndex) {
                        AnimChannel declared;
                        declared.id = owner.animChannels.nextFreeId();
                        declared.target.kind = TrackTarget::Kind::Node;
                        declared.target.node = bones[b];
                        declared.target.channel = which;
                        declared.valueType = slot == 0 ? geom::AttrType::F32x3 : geom::AttrType::Quat;
                        boneChannels[b][slot] = owner.animChannels.add(declared);
                    }
                }
            }
            // Clip by clip, side by side: each writes its own clip alone.
            geom::fracture::Parallel(sampled.size(), threads, [&](std::size_t job) {
                const Sampled& s = sampled[job];
                Clip& clip = document.clips[s.clip];
                if (clip.containers.empty()) {
                    clip.containers.emplace_back();
                }
                // A made bone's parent is the holder, the one frame its local
                // reads.
                Pose holding;
                holding.frame.assign(tree.size(), Matrix44f::identity());
                holding.local.assign(tree.size(), Transform{});
                for (u32 b = 0; b < bones.size(); ++b) {
                    std::vector<Vector3f> moves;
                    std::vector<Quaternion> turns;
                    for (std::size_t i = 0; i < s.frames.size(); ++i) {
                        if (drivers.holder < tree.size()) {
                            holding.frame[drivers.holder] = s.holder[i];
                        }
                        const Transform local = StageLocalFor(tree, holding, bones[b], s.frames[i][b]);
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
                            Thin(turns, tolerance / std::max(lever[b], 1e-6f), [](const Quaternion& a, const Quaternion& b, f32 u) {
                                     return Quaternion::slerp(a, b, u);
                                 },
                                 [](const Quaternion& a, const Quaternion& b) {
                                     return 2.0f * std::acos(std::min(1.0f, std::abs(a.dot(b))));
                                 },
                                 keys);
                        }
                        SubTrack track;
                        track.channel = boneChannels[b][move ? 0 : 1];
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
            });
            made += static_cast<u32>(bones.size());
            char said[96] = "";
            if (fits) {
                std::snprintf(said, sizeof said, "' %zu particles, within %.2f RMS", drivers.vertices.size(),
                              static_cast<double>(fitRms));
            }
            out.info(DiagCode::PhysicsUnsupported,
                     stem + ": " + std::to_string(bones.size()) + " bones made from its drivers" + said +
                         ", the target running no cloth",
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
    out.scale = Vector3f{1, 1, 1};
    return skinning::SolveRigid(a, b, s, out.rotation, out.translation);
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
