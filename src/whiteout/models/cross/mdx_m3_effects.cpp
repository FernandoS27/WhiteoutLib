// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/cross/mdx_m3_effects.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "../wem/converters/m3_anim.h"
#include "../wem/converters/m3_track_sink.h"
#include "../wem/converters/mdx_track_slicer.h"

namespace whiteout {
namespace models {
namespace cross {

namespace {

namespace wem = ::whiteout::models::wem;
namespace m3_sink = wem::m3_sink;
namespace mdx_slice = wem::mdx_slice;
using wem::DiagCode;
using wem::ElementKind;
using wem::ElementRef;
using wem::kInvalidIndex;
using wem::ValuesPerKey;

template <class T>
void Rest(m3::AnimRef<T>& ref, const T& value) {
    ref.initValue = value;
    ref.nullValue = value;
}

/// The node the `.mdx` numbered @p objectId.
u32 NodeOfObjectId(const wem::Document& document, u32 objectId) {
    if (document.models.empty()) {
        return kInvalidIndex;
    }
    const wem::NodeTree& nodes = document.models.front().nodes;
    for (u32 n = 0; n < nodes.size(); ++n) {
        const wem::NodeNative& native = nodes.nodes[n].native;
        if (native.find("objectId") != nullptr &&
            native.value("objectId", -1) == static_cast<i64>(objectId)) {
            return n;
        }
    }
    return kInvalidIndex;
}

/// @p track at @p timeMs on its own timeline, the value element only: held
/// before the first key and after the last, held across a step span, a
/// straight line otherwise. A smooth key's tangents are not read -- no M3
/// stream carries them.
template <class T>
T SampleTrack(const mdx::Track<T>& track, f32 timeMs, const T& rest) {
    if (!track.isUsed || track.timestamps.empty() || !mdx_slice::WellFormed(track)) {
        return rest;
    }
    const u32 perKey = ValuesPerKey(
        mdx_slice::InterpOf(track.interpolationType, mdx_slice::ValueTrait<T>::kType));
    const auto value = [&](std::size_t k) { return track.keys_data[k * perKey]; };
    const std::vector<u32>& t = track.timestamps;
    if (timeMs <= static_cast<f32>(t.front())) {
        return value(0);
    }
    if (timeMs >= static_cast<f32>(t.back())) {
        return value(t.size() - 1);
    }
    std::size_t hi = 1;
    while (hi < t.size() && static_cast<f32>(t[hi]) < timeMs) {
        ++hi;
    }
    if (track.interpolationType == mdx::InterpolationType::None || t[hi] == t[hi - 1]) {
        return static_cast<f32>(t[hi]) <= timeMs ? value(hi) : value(hi - 1);
    }
    const f32 a = static_cast<f32>(t[hi - 1]);
    const f32 f = (timeMs - a) / (static_cast<f32>(t[hi]) - a);
    const T lo = value(hi - 1);
    const T up = value(hi);
    return lo + (up - lo) * f;
}

bool Keyed(const auto& track) {
    return track.isUsed && !track.timestamps.empty();
}

// ----------------------------------------------------------------------------
// Streams: an mdx track, cut into the clips, into the file
// ----------------------------------------------------------------------------

class Streams {
public:
    Streams(const wem::Document& staged, const wem::M3ExportMap& map, m3::Model& out,
            Wc3EffectReport& report)
        : staged_(staged), map_(map), out_(out), report_(report), next_(map.nextAnimId) {
        // Past every id the file already names, whatever the map said.
        for (const m3::SubTrackContainer& stc : out.subTrackCollections) {
            for (const u32 id : stc.animIds) {
                next_ = (std::max)(next_, id + 1);
            }
        }
        next_ = (std::max)(next_, u32{1});
    }

    /// Writes @p track (values already in the StarCraft II property's units
    /// and layout, `f32`, `u32` or `Vector3f`) into every clip that plays it,
    /// and binds every AnimRef in @p refs to the one stream. False when no clip
    /// plays a key of it -- the AnimRefs then rest at their own values.
    template <class T, class Ref>
    bool keyed(const mdx::Track<T>& track, m3_sink::Stream stream,
               std::initializer_list<m3::AnimRef<Ref>*> refs,
               const Quaternion* restQuaternion = nullptr) {
        const std::vector<mdx_slice::ClipCut> cuts =
            mdx_slice::CutForClips(track, staged_, 0, 0);
        if (cuts.empty()) {
            return false;
        }
        const u32 id = next_;
        bool written = false;
        for (const mdx_slice::ClipCut& cut : cuts) {
            u32 stc = kInvalidIndex;
            u32 sequence = kInvalidIndex;
            if (!containerOf(cut.clip, sequence, stc)) {
                continue;
            }
            m3_sink::StreamSpec spec;
            spec.stream = stream;
            spec.type = mdx_slice::ValueTrait<T>::kType;
            spec.restQuaternion = restQuaternion;
            const u32 ref = m3_sink::WriteStream(
                out_.subTrackCollections[stc], spec, cut.track,
                static_cast<i32>(out_.sequences[sequence].startFrame),
                staged_.clips[cut.clip].duration,
                m3_sink::WarcraftWindow(staged_.clips[cut.clip]));
            if (ref == kInvalidIndex) {
                continue;
            }
            m3_sink::AddToContainer(out_, stc, id, ref);
            for (m3::AnimRef<Ref>* anim : refs) {
                wiring_.wire(*anim, id, cut.track.interp);
            }
            ++report_.keyedStreams;
            written = true;
        }
        if (written) {
            ++next_;
        }
        return written;
    }

private:
    bool containerOf(u32 clip, u32& sequence, u32& stc) const {
        if (clip >= map_.clipSequence.size()) {
            return false;
        }
        sequence = map_.clipSequence[clip];
        if (sequence >= map_.sequenceStc.size() || sequence >= out_.sequences.size()) {
            return false;
        }
        stc = map_.sequenceStc[sequence];
        return stc < out_.subTrackCollections.size();
    }

    const wem::Document& staged_;
    const wem::M3ExportMap& map_;
    m3::Model& out_;
    Wc3EffectReport& report_;
    u32 next_ = 1;
    m3_sink::Wiring wiring_;
};

// ----------------------------------------------------------------------------
// Cameras
// ----------------------------------------------------------------------------

/// A Warcraft III point in the written model's basis and units.
Vector3f Restate(const Vector3f& v, f32 lengthScale) {
    return Vector3f{v.y * lengthScale, -v.x * lengthScale, v.z * lengthScale};
}

Vector3f Cross3(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

f32 Length3(const Vector3f& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// The bone frame a StarCraft II camera at @p position sees @p target from:
/// it looks down -Z, X lies level (world +Z up), Y = Z x X, and the whole
/// frame turns @p roll radians about Z. Blizzard's conversions state exactly
/// this -- Z on 398 of 400 portraits, the whole frame on 368, and the roll with
/// Warcraft III's sign on every rolled camera. Rows X, Y, Z and the position,
/// the way `ToMatrix` lays a transform out. False when the two points meet.
bool CameraFrame(const Vector3f& position, const Vector3f& target, f32 roll, Matrix44f& out) {
    const Vector3f toward{target.x - position.x, target.y - position.y, target.z - position.z};
    const f32 length = Length3(toward);
    if (!(length > 1e-6f)) {
        return false;
    }
    const Vector3f z{-toward.x / length, -toward.y / length, -toward.z / length};
    Vector3f x = Cross3(Vector3f{0, 0, 1}, z);
    if (Length3(x) < 1e-6f) {
        // Straight up or down: level has no direction, so +Y's cross stands in.
        x = Cross3(Vector3f{0, 1, 0}, z);
    }
    const f32 xl = Length3(x);
    x = Vector3f{x.x / xl, x.y / xl, x.z / xl};
    const Vector3f y = Cross3(z, x);
    const f32 c = std::cos(roll);
    const f32 s = std::sin(roll);
    const Vector3f rx{x.x * c + y.x * s, x.y * c + y.y * s, x.z * c + y.z * s};
    const Vector3f ry{y.x * c - x.x * s, y.y * c - x.y * s, y.z * c - x.z * s};
    out = Matrix44f::identity();
    const std::array<Vector3f, 4> rows{rx, ry, z, position};
    for (int r = 0; r < 4; ++r) {
        out.data[r][0] = rows[r].x;
        out.data[r][1] = rows[r].y;
        out.data[r][2] = rows[r].z;
    }
    return true;
}

class CameraCrossing {
public:
    CameraCrossing(const mdx::Model& source, const wem::Document& staged,
                   const wem::M3ExportMap& map, const Wc3EffectOptions& options, m3::Model& out,
                   Streams& streams, Wc3EffectReport& report)
        : source_(source), staged_(staged), map_(map), options_(options), out_(out),
          streams_(streams), report_(report) {}

    void run() {
        // `toM3` writes a CAM_ per camera node in node order, and the import
        // appends the `.mdx` cameras as the document's camera nodes in theirs.
        std::vector<u32> nodes;
        if (!staged_.models.empty()) {
            const wem::NodeTree& tree = staged_.models.front().nodes;
            for (u32 n = 0; n < tree.size(); ++n) {
                if (tree.nodes[n].kind == wem::NodeKind::Camera) {
                    nodes.push_back(n);
                }
            }
        }
        for (std::size_t c = 0; c < source_.cameras.size(); ++c) {
            if (c >= nodes.size() || c >= out_.cameras.size()) {
                break;
            }
            cross(source_.cameras[c], nodes[c], out_.cameras[c]);
        }
    }

private:
    void cross(const mdx::Camera& camera, u32 node, m3::Camera& record) {
        const f32 L = options_.lengthScale;
        const ElementRef where(ElementKind::Node, node);

        // Warcraft III states a horizontal field of view over a 4:3 frame;
        // Blizzard's conversions write the vertical one it implies, and say so.
        Rest(record.fieldOfView, 2.0f * std::atan(0.75f * std::tan(camera.fieldOfView * 0.5f)));
        record.useVerticalFOV = 1;
        Rest(record.nearClip, camera.nearClippingPlane * L);
        Rest(record.farClip, camera.farClippingPlane * L);
        // The depth-of-field rests Blizzard's tool writes on every one of them,
        // in the written model's units.
        Rest(record.shadowClipDistance, 20.0f);
        Rest(record.focusDistance, 5.0f * L);
        Rest(record.farFocusRange, 1.0f * L);
        Rest(record.nearFocusRange, 2.0f * L);

        const u32 bone = node < map_.nodeBone.size() ? map_.nodeBone[node] : kInvalidIndex;
        if (bone == kInvalidIndex || bone >= out_.bones.size() || record.boneIndex != bone ||
            out_.bones[bone].parentIndex != 0xFFFFu) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "camera '" + camera.name +
                                         "' has no bone of its own; it looks down the one it "
                                         "rides, not at its target",
                                     where);
            return;
        }
        m3::Bone& carrier = out_.bones[bone];

        // At rest: the bone `toM3` placed, turned toward the target.
        const Vector3f position = carrier.position.initValue;
        Matrix44f frame;
        if (!CameraFrame(position, Restate(camera.targetPosition, L), 0.0f, frame)) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "camera '" + camera.name +
                                         "' sits on its own target; it keeps the bone's rotation",
                                     where);
            return;
        }
        const wem::Transform rest = wem::FromMatrix(frame);
        Rest(carrier.rotation, rest.rotation);
        if (bone < out_.initialReference.size()) {
            out_.initialReference[bone].matrix = Matrix44f::inverse(frame);
        }
        ++report_.cameraRecords;

        // Moving: the frame sampled on the union of the keys that move either
        // end or the roll, one rotation key each.
        u32 clock = mdx_slice::kNoGlobalSequence;
        bool clockSet = false;
        bool approximated = false;
        std::vector<u32> times;
        const auto gather = [&](const auto& track) {
            if (!Keyed(track) || !mdx_slice::WellFormed(track)) {
                return false;
            }
            if (clockSet && track.globalSequenceId != clock) {
                approximated = true;
                return false;
            }
            clock = track.globalSequenceId;
            clockSet = true;
            times.insert(times.end(), track.timestamps.begin(), track.timestamps.end());
            return true;
        };
        const bool usePosition = gather(camera.positionTracks);
        const bool useTarget = gather(camera.targetPositionTracks);
        const bool useRoll = gather(camera.targetRotationTracks);
        if (approximated) {
            report_.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                     "camera '" + camera.name +
                                         "' keys its position, target and roll on different "
                                         "clocks; the aim follows the first",
                                     where);
        }
        if (times.empty()) {
            return;
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        const auto stepped = [](bool used, const auto& track) {
            return !used || track.interpolationType == mdx::InterpolationType::None;
        };

        mdx::Track<Quaternion> aim;
        aim.isUsed = true;
        aim.globalSequenceId = clock;
        aim.interpolationType = stepped(usePosition, camera.positionTracks) &&
                                        stepped(useTarget, camera.targetPositionTracks) &&
                                        stepped(useRoll, camera.targetRotationTracks)
                                    ? mdx::InterpolationType::None
                                    : mdx::InterpolationType::Linear;
        const Vector3f still{0, 0, 0};
        for (const u32 time : times) {
            const f32 at = static_cast<f32>(time);
            // KCTR and KTTR move each end off its rest; KCRL is the roll itself.
            const Vector3f moved = usePosition ? SampleTrack(camera.positionTracks, at, still) : still;
            const Vector3f aimed =
                useTarget ? SampleTrack(camera.targetPositionTracks, at, still) : still;
            const f32 roll = useRoll ? SampleTrack(camera.targetRotationTracks, at, 0.0f) : 0.0f;
            Matrix44f keyFrame;
            const bool framed = CameraFrame(Restate(camera.position + moved, L),
                                            Restate(camera.targetPosition + aimed, L), roll,
                                            keyFrame);
            aim.timestamps.push_back(time);
            aim.keys_data.push_back(framed ? wem::FromMatrix(keyFrame).rotation : rest.rotation);
        }
        aim.keyCount = aim.timestamps.size();
        // WEM holds no camera target, so this is the one place a crossing
        // turns a bone `toM3` made: the rest, its IREF and its rotation stream.
        // The file latches its bone flags as solved, so a newly keyed bone is
        // solved again or the engine plays it frozen.
        if (streams_.keyed(aim, m3_sink::Stream::Sd4q, {&carrier.rotation},
                           &carrier.rotation.initValue)) {
            wem::m3_anim::SolveBoneAnimFlags(out_);
        }
    }

    const mdx::Model& source_;
    const wem::Document& staged_;
    const wem::M3ExportMap& map_;
    const Wc3EffectOptions& options_;
    m3::Model& out_;
    Streams& streams_;
    Wc3EffectReport& report_;
};

// ----------------------------------------------------------------------------
// CLID -> MODL fuzzy hit tests (C8.2)
// ----------------------------------------------------------------------------

/// Each Warcraft III collision shape as the fuzzy hit test StarCraft II picks a
/// model with, on the shape node's own bone, which rests at its pivot. A
/// sphere states its centre in model space and a box its corners about the
/// pivot (AzureDragon's three boxes all centre on (0, 0, z), one per pivot).
/// Blizzard's conversions put 523 of 550 sphere tests on a bone at the centre
/// sized by the radius (532), and 151 of 174 box tests on a bone at the pivot
/// with sizes scaled by hand in Max. An M3 box's sizes are half extents.
void CrossHitTests(const mdx::Model& source, const wem::Document& staged,
                   const wem::M3ExportMap& map, const Wc3EffectOptions& options, m3::Model& out,
                   Wc3EffectReport& report) {
    const f32 L = options.lengthScale;
    for (const mdx::CollisionShape& shape : source.collisionShapes) {
        const std::string& name = shape.node.name;
        const ElementRef where(ElementKind::Node, shape.node.objectId);
        const u32 node = NodeOfObjectId(staged, shape.node.objectId);
        const u32 bone = node < map.nodeBone.size() ? map.nodeBone[node] : kInvalidIndex;
        if (bone == kInvalidIndex || bone >= out.bones.size()) {
            report.diagnostics.warn(DiagCode::FeatureDropped,
                                    "collision shape '" + name +
                                        "' has no bone to ride; its hit test was not written",
                                    where);
            continue;
        }
        const Vector3f pivot = shape.node.objectId < source.pivotPoints.size()
                                   ? source.pivotPoints[shape.node.objectId]
                                   : Vector3f{0, 0, 0};
        m3::HitTestShape hit;
        hit.boneIndex = static_cast<u16>(bone);
        Vector3f offset{0, 0, 0};
        if (shape.type == mdx::CollisionShape::ShapeType::Sphere && !shape.vertices.empty()) {
            hit.shapeType = m3::HitTestShapeType::Sphere;
            hit.sizeX = shape.radius * L;
            offset = Vector3f{shape.vertices[0].x - pivot.x, shape.vertices[0].y - pivot.y,
                              shape.vertices[0].z - pivot.z};
        } else if (shape.type == mdx::CollisionShape::ShapeType::Box &&
                   shape.vertices.size() >= 2) {
            const Vector3f& a = shape.vertices[0];
            const Vector3f& b = shape.vertices[1];
            hit.shapeType = m3::HitTestShapeType::Box;
            // The basis change swaps the two ground axes.
            hit.sizeX = 0.5f * std::fabs(b.y - a.y) * L;
            hit.sizeY = 0.5f * std::fabs(b.x - a.x) * L;
            hit.sizeZ = 0.5f * std::fabs(b.z - a.z) * L;
            offset = Vector3f{0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z)};
        } else {
            report.diagnostics.warn(DiagCode::FeatureDropped,
                                    "collision shape '" + name +
                                        "' is not a sphere or a box; no hit test was written",
                                    where);
            continue;
        }
        const Vector3f at = Restate(offset, L);
        hit.transform.data[3][0] = at.x;
        hit.transform.data[3][1] = at.y;
        hit.transform.data[3][2] = at.z;
        out.fuzzyHitTestObjects.push_back(std::move(hit));
        ++report.hitTests;
    }
}

} // namespace

Wc3EffectReport CrossWc3Effects(const mdx::Model& source, const wem::Document& staged,
                                const wem::M3ExportMap& map, const Wc3EffectOptions& options,
                                m3::Model& out) {
    Wc3EffectReport report;
    Streams streams(staged, map, out, report);
    if (options.cameras) {
        CameraCrossing(source, staged, map, options, out, streams, report).run();
    }
    if (options.hitTests) {
        CrossHitTests(source, staged, map, options, out, report);
    }
    // C8.3: Warcraft III's event objects are sound, splat and footprint
    // triggers the game looks up by name in its SLKs. They cross as named event
    // keys (the timing survives), but StarCraft II plays sounds and splats from
    // actor data, so they fire nothing on their own; Blizzard's conversions
    // dropped every one (0 of 912 models keep a source event).
    if (!source.eventObjects.empty()) {
        report.diagnostics.info(DiagCode::FeatureDropped,
                                std::to_string(source.eventObjects.size()) +
                                    " event object(s) (EVTS) crossed as named model events; "
                                    "StarCraft II plays sounds and splats from actor data",
                                ElementRef());
    }
    return report;
}

} // namespace cross
} // namespace models
} // namespace whiteout
