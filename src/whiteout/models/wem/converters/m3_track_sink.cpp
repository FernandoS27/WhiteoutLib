// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_track_sink.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_sink {

namespace {

/// The value a Warcraft III track shows at fraction @p t of its wrap segment
/// from @p last back to @p first, as @p size bytes of @p type. A step track
/// holds the last key across it; a quaternion takes the short way round.
std::vector<u8> WrapValue(geom::AttrType type, Interpolation interp, const u8* last,
                          const u8* first, f32 t, std::size_t size) {
    std::vector<u8> out(last, last + size);
    if (interp == Interpolation::Step) {
        return out;
    }
    switch (type) {
    case geom::AttrType::F32:
    case geom::AttrType::F32x2:
    case geom::AttrType::F32x3:
    case geom::AttrType::F32x4: {
        const std::size_t n = size / sizeof(f32);
        for (std::size_t i = 0; i < n; ++i) {
            f32 a = 0, b = 0;
            std::memcpy(&a, last + i * sizeof(f32), sizeof(f32));
            std::memcpy(&b, first + i * sizeof(f32), sizeof(f32));
            const f32 v = a + (b - a) * t;
            std::memcpy(out.data() + i * sizeof(f32), &v, sizeof(f32));
        }
        return out;
    }
    case geom::AttrType::Quat: {
        Quaternion a{}, b{};
        std::memcpy(&a, last, sizeof(a));
        std::memcpy(&b, first, sizeof(b));
        const f32 sign = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f ? -1.0f : 1.0f;
        Quaternion v{a.x + (sign * b.x - a.x) * t, a.y + (sign * b.y - a.y) * t,
                     a.z + (sign * b.z - a.z) * t, a.w + (sign * b.w - a.w) * t};
        const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
        if (length > 0.0f) {
            v = Quaternion{v.x / length, v.y / length, v.z / length, v.w / length};
        }
        std::memcpy(out.data(), &v, sizeof(v));
        return out;
    }
    default:
        return out;
    }
}

/// Keeps a quaternion stream in one hemisphere: every key is flipped onto the
/// side of the one before it, and the first onto the side of @p reference
/// where there is one. Retail's `M3Anim_EvalTrackQuat` lerps the four
/// components as stored -- no slerp, no sign test -- so a stream whose keys
/// alternate q, -q (which the Warcraft III Max exporter writes, and which
/// WC3's own slerp absorbs) passes near zero between every pair of keys: the
/// bone shrinks and swings at key rate, a tremor. Blizzard's own War3_*.m3
/// conversions carry no such pair.
void AlignHemispheres(std::vector<Quaternion>& keys, const Quaternion* reference) {
    const auto flipOnto = [](const Quaternion& onto, Quaternion& q) {
        if (onto.x * q.x + onto.y * q.y + onto.z * q.z + onto.w * q.w < 0.0f) {
            q = Quaternion{-q.x, -q.y, -q.z, -q.w};
        }
    };
    if (keys.empty()) {
        return;
    }
    if (reference != nullptr) {
        flipOnto(*reference, keys.front());
    }
    for (std::size_t k = 1; k < keys.size(); ++k) {
        flipOnto(keys[k - 1], keys[k]);
    }
}

/// Fills a rotation stream in so no span asks the engine's lerp to cross more
/// than @p kMaxSpanDegrees of turn.
///
/// Retail builds a bone's basis from the sampled quaternion with the unit
/// formula and no normalise (`M3_UpdateNodeTransform`), and the sampler lerps
/// the four components raw. A lerp across a wide span passes inside the unit
/// sphere, so the basis SHRINKS mid-span -- by |q|^2 = cos^2(span/4), which is
/// 15% at 90 degrees and 45% at 169. Warcraft III slerps, so its own files say
/// a turn in as few keys as they like: Zhao Yun's ground discs spin on five
/// keys a quarter turn apart and pulsed once per key. Shipped content never
/// asks for it -- of 5,280,791 adjacent pairs in 18,409 Heroes models not one
/// exceeds 28.1 degrees, and 99.87% of StarCraft II's 25,603,617 stay inside
/// 30 -- because it is authored dense. So the keys the engine reads are the
/// arc, sampled: the turn is unchanged and every span it holds is one the
/// lerp can carry (1.7% at 30 degrees, below a byte of colour).
void SubdivideRotations(std::vector<i32>& stamps, std::vector<Quaternion>& keys) {
    constexpr f32 kMaxSpanDegrees = 30.0f;
    constexpr f32 kMaxSpanDot = 0.96592f; // cos(30 / 2 degrees): the quaternion half-angle.
    if (keys.size() < 2 || keys.size() != stamps.size()) {
        return;
    }
    std::vector<i32> outStamps;
    std::vector<Quaternion> outKeys;
    outStamps.reserve(keys.size());
    outKeys.reserve(keys.size());
    for (std::size_t k = 0; k + 1 < keys.size(); ++k) {
        outStamps.push_back(stamps[k]);
        outKeys.push_back(keys[k]);
        const Quaternion& a = keys[k];
        const Quaternion& b = keys[k + 1];
        // Hemispheres are aligned above, so the dot is the short way round.
        const f32 dot = std::clamp(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w, -1.0f, 1.0f);
        if (dot >= kMaxSpanDot) {
            continue;
        }
        const f32 half = std::acos(dot);                                  // radians, half the turn
        const f32 degrees = half * 2.0f * 180.0f / 3.14159265358979f;
        const i32 span = stamps[k + 1] - stamps[k];
        // A stamp is a millisecond and two keys may not share one: a span too
        // short to hold the pieces keeps as many as it can.
        const auto wanted = static_cast<i32>(std::ceil(degrees / kMaxSpanDegrees));
        const i32 pieces = std::min<i32>(wanted, span);
        const f32 sinHalf = std::sin(half);
        if (pieces < 2 || sinHalf <= 1e-6f) {
            continue;
        }
        for (i32 piece = 1; piece < pieces; ++piece) {
            const f32 t = static_cast<f32>(piece) / static_cast<f32>(pieces);
            const f32 wa = std::sin((1.0f - t) * half) / sinHalf;
            const f32 wb = std::sin(t * half) / sinHalf;
            outStamps.push_back(stamps[k] + static_cast<i32>(std::lround(t * span)));
            outKeys.push_back(Quaternion{a.x * wa + b.x * wb, a.y * wa + b.y * wb,
                                         a.z * wa + b.z * wb, a.w * wa + b.w * wb});
        }
    }
    outStamps.push_back(stamps.back());
    outKeys.push_back(keys.back());
    stamps = std::move(outStamps);
    keys = std::move(outKeys);
}

/// The inverse of `m3_converter`'s `Rebase` -- WEM's canonical basis back into SC2's.
Vector3f Unrebase(const Vector3f& v) {
    return Vector3f{v.y, -v.x, v.z};
}

Quaternion UnrebaseRotation(const Quaternion& q) {
    return Quaternion{q.y, -q.x, q.z, q.w};
}

m3::ColorBGRA FromRgba(const Vector4f& value) {
    const auto byteOf = [](f32 v) {
        const f32 scaled = v * 255.0f;
        return static_cast<u8>(scaled <= 0.0f ? 0.0f : scaled >= 255.0f ? 255.0f : scaled + 0.5f);
    };
    m3::ColorBGRA color;
    color.r = byteOf(value.x);
    color.g = byteOf(value.y);
    color.b = byteOf(value.z);
    color.a = byteOf(value.w);
    return color;
}

template <class T>
m3::AnimBlock<T> NewBlock(const std::vector<i32>& stamps, u32 endFrame) {
    m3::AnimBlock<T> block;
    block.timestamps = stamps;
    block.flags = 0;
    block.endFrame = endFrame;
    return block;
}

/// @p comps floats of key @p key of @p track, the value element only (a smooth
/// key stores tangents beside it).
void KeyValue(const SubTrack& track, u32 comps, std::size_t key, f32* out) {
    const std::size_t stride = static_cast<std::size_t>(ValuesPerKey(track.interp)) * comps;
    std::memcpy(out, track.values.data() + key * stride * sizeof(f32),
                static_cast<std::size_t>(comps) * sizeof(f32));
}

/// @p track sampled at @p time: held before its first key and after its last,
/// held across a step span, and a straight line between any other pair.
void SampleAt(const SubTrack& track, u32 comps, f32 time, f32* out) {
    const std::size_t count = track.times.size();
    if (count == 0) {
        return;
    }
    if (time <= track.times.front()) {
        KeyValue(track, comps, 0, out);
        return;
    }
    if (time >= track.times.back()) {
        KeyValue(track, comps, count - 1, out);
        return;
    }
    std::size_t hi = 1;
    while (hi < count && track.times[hi] < time) {
        ++hi;
    }
    f32 a[4] = {};
    f32 b[4] = {};
    KeyValue(track, comps, hi - 1, a);
    KeyValue(track, comps, hi, b);
    const f32 span = track.times[hi] - track.times[hi - 1];
    const f32 t = track.interp == Interpolation::Step || span <= 0.0f
                      ? 0.0f
                      : (time - track.times[hi - 1]) / span;
    for (u32 c = 0; c < comps; ++c) {
        out[c] = a[c] + (b[c] - a[c]) * t;
    }
}

} // namespace

i32 Ticks(f32 seconds) {
    return static_cast<i32>(seconds * 1000.0f + (seconds < 0.0f ? -0.5f : 0.5f));
}

bool WarcraftWindow(const Clip& clip) {
    return clip.native.value("intervalStart", -1) >= 0 ||
           clip.native.value("globalSequenceId", -1) >= 0;
}

u32 WriteStream(m3::SubTrackContainer& stc, const StreamSpec& spec, const SubTrack& track,
                i32 origin, f32 duration, bool warcraft) {
    if (spec.stream == Stream::None) {
        return kInvalidIndex;
    }
    const std::size_t size = geom::AttrTypeSize(spec.type);
    const std::size_t stride = ValuesPerKey(track.interp) * size;
    const auto at = [&](std::size_t key) { return track.values.data() + key * stride; };

    // Which keys the sequence plays.
    //
    // The MDX slicer keeps one bracketing key past each edge of a clip so
    // the wem player can interpolate the edge spans. A SEQS timeline
    // cannot say that: a key before the clip becomes the value in effect
    // at its start (the LAST such key, held at the origin), and a key
    // past its end is dropped -- the sampler then answers the last
    // in-range key, which is the hold the global timeline plays. Left
    // in, an out-of-window key sits at a timestamp the sequence never
    // reaches and a whole channel reads as its bracket value: the
    // Grunt's every geoset gated itself invisible on a key 139 seconds
    // past `Stand 01`.
    //
    // A Warcraft III window is stricter still. Its engine reads only the
    // keys inside the window -- the bracket keys are keys it never sees --
    // and plays the span after the last one, and the span before the
    // first, as one segment from the last key back to the first. M3 holds
    // before its first key and wraps a looping track on its own last
    // stamp, so a track that does not reach an edge gets a key there
    // carrying the value the engine shows at it: the start and end key
    // Warcraft III has on every track, restated for a format that has
    // defaults. A track with no key inside the window is not written;
    // the engine answers its rest value, and so does the AnimRef's own.
    std::vector<std::size_t> kept;
    std::ptrdiff_t entry = -1;
    const f32 slack = duration > 0 ? duration : track.times.empty() ? 0 : track.times.back();
    for (std::size_t k = 0; k < track.times.size(); ++k) {
        const f32 time = track.times[k];
        if (time < -1e-4f) {
            entry = static_cast<std::ptrdiff_t>(k);
        } else if (time <= slack + 1e-4f) {
            kept.push_back(k);
        }
    }
    // Not a squirt's: SDS6 holds nothing, it fires a burst as the playhead
    // crosses a key, and a key before the window is one this sequence never
    // crosses. Folded onto the origin it would fire at every start.
    if (entry >= 0 && !warcraft && spec.stream != Stream::Sds6 &&
        (kept.empty() || Ticks(track.times[kept.front()]) > 0)) {
        kept.insert(kept.begin(), static_cast<std::size_t>(entry));
    }
    if (kept.empty()) {
        return kInvalidIndex;
    }

    std::vector<u8> wrap;
    bool wrapStart = false;
    bool wrapEnd = false;
    if (warcraft && kept.size() >= 2 && duration > 0.0f) {
        const f32 firstTime = track.times[kept.front()];
        const f32 lastTime = track.times[kept.back()];
        wrapStart = Ticks(firstTime) > 0;
        wrapEnd = Ticks(lastTime) < Ticks(duration);
        if (wrapStart || wrapEnd) {
            // The same point of the wrap segment either way: the sequence
            // end IS its start, one loop on.
            const f32 segment = (firstTime - lastTime) + duration;
            const f32 t =
                segment > 0.0f ? std::clamp((duration - lastTime) / segment, 0.0f, 1.0f) : 0.0f;
            wrap = WrapValue(spec.type, track.interp, at(kept.back()), at(kept.front()), t, size);
        }
    }

    std::vector<i32> stamps;
    std::vector<const u8*> values;
    stamps.reserve(kept.size() + 2);
    values.reserve(kept.size() + 2);
    if (wrapStart) {
        stamps.push_back(origin);
        values.push_back(wrap.data());
    }
    for (std::size_t k : kept) {
        const f32 time = track.times[k] < 0.0f ? 0.0f : track.times[k];
        stamps.push_back(origin + Ticks(time));
        values.push_back(at(k));
    }
    if (wrapEnd) {
        stamps.push_back(origin + Ticks(duration));
        values.push_back(wrap.data());
    }
    const std::size_t count = stamps.size();
    // Every block ends where its sequence does -- 61,344 of 61,344 shipped
    // blocks, the 1,550 single-key ones included -- never at its last key.
    // Ended at its last key, each hidden Grunt geoset was one key of 0 at
    // frame 0 ending at 0, and the Galaxy editor drew every one of them.
    const auto endFrame = static_cast<u32>(origin + Ticks(duration));

    u32 block = 0;
    switch (spec.stream) {
    case Stream::Sd2v: {
        auto out = NewBlock<Vector2f>(stamps, endFrame);
        for (std::size_t k = 0; k < count; ++k) {
            Vector2f value{};
            std::memcpy(&value, values[k], sizeof(value));
            out.keys.push_back(value);
        }
        block = static_cast<u32>(stc.sd2v.size());
        stc.sd2v.push_back(std::move(out));
        break;
    }
    case Stream::Sd3v: {
        auto out = NewBlock<Vector3f>(stamps, endFrame);
        for (std::size_t k = 0; k < count; ++k) {
            Vector3f value{};
            std::memcpy(&value, values[k], sizeof(value));
            out.keys.push_back(spec.unrebaseVector ? Unrebase(value) : value);
        }
        block = static_cast<u32>(stc.sd3v.size());
        stc.sd3v.push_back(std::move(out));
        break;
    }
    case Stream::Sd4q: {
        auto out = NewBlock<Quaternion>(stamps, endFrame);
        for (std::size_t k = 0; k < count; ++k) {
            Quaternion value{};
            std::memcpy(&value, values[k], sizeof(value));
            out.keys.push_back(spec.unrebaseQuaternion ? UnrebaseRotation(value) : value);
        }
        AlignHemispheres(out.keys, spec.restQuaternion);
        if (track.interp != Interpolation::Step) {
            SubdivideRotations(out.timestamps, out.keys);
        }
        block = static_cast<u32>(stc.sd4q.size());
        stc.sd4q.push_back(std::move(out));
        break;
    }
    case Stream::Sdcc: {
        auto out = NewBlock<m3::ColorBGRA>(stamps, endFrame);
        for (std::size_t k = 0; k < count; ++k) {
            Vector4f value{};
            std::memcpy(&value, values[k], sizeof(value));
            out.keys.push_back(FromRgba(value));
        }
        block = static_cast<u32>(stc.sdcc.size());
        stc.sdcc.push_back(std::move(out));
        break;
    }
    case Stream::Sdr3: {
        auto out = NewBlock<f32>(stamps, endFrame);
        for (std::size_t k = 0; k < count; ++k) {
            f32 value = 0.0f;
            std::memcpy(&value, values[k], sizeof(value));
            out.keys.push_back(value);
        }
        block = static_cast<u32>(stc.sdr3.size());
        stc.sdr3.push_back(std::move(out));
        break;
    }
    case Stream::Sds6: {
        // A count: the runtime reads the slot as u16, and every shipped value
        // fits the file's i16.
        auto out = NewBlock<i16>(stamps, endFrame);
        for (std::size_t k = 0; k < count; ++k) {
            u32 value = 0;
            std::memcpy(&value, values[k], sizeof(value));
            out.keys.push_back(static_cast<i16>((std::min)(value, u32{0x7FFF})));
        }
        block = static_cast<u32>(stc.sds6.size());
        stc.sds6.push_back(std::move(out));
        break;
    }
    case Stream::Sdu3: {
        auto out = NewBlock<u32>(stamps, endFrame);
        for (std::size_t k = 0; k < count; ++k) {
            u32 value = 0;
            std::memcpy(&value, values[k], sizeof(value));
            out.keys.push_back(value);
        }
        block = static_cast<u32>(stc.sdu3.size());
        stc.sdu3.push_back(std::move(out));
        break;
    }
    case Stream::Sdfg: {
        auto out = NewBlock<m3::Flag>(stamps, endFrame);
        for (std::size_t k = 0; k < count; ++k) {
            f32 value = 0.0f;
            std::memcpy(&value, values[k], sizeof(value));
            m3::Flag flag{};
            flag.value = value != 0.0f ? 1u : 0u;
            out.keys.push_back(flag);
        }
        block = static_cast<u32>(stc.sdfg.size());
        stc.sdfg.push_back(std::move(out));
        break;
    }
    case Stream::None:
        return kInvalidIndex;
    }
    return (static_cast<u32>(spec.stream) << 16) | block;
}

void SortLookup(m3::SubTrackContainer& stc) {
    std::vector<std::size_t> order(stc.animIds.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
        order[k] = k;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return stc.animIds[a] < stc.animIds[b];
    });
    std::vector<u32> ids;
    std::vector<u32> refs;
    ids.reserve(order.size());
    refs.reserve(order.size());
    for (const std::size_t k : order) {
        ids.push_back(stc.animIds[k]);
        refs.push_back(stc.animRefs[k]);
    }
    stc.animIds = std::move(ids);
    stc.animRefs = std::move(refs);
}

void AddToContainer(m3::Model& out, u32 stcIndex, u32 animId, u32 animRef) {
    if (stcIndex >= out.subTrackCollections.size()) {
        return;
    }
    m3::SubTrackContainer& stc = out.subTrackCollections[stcIndex];
    stc.animIds.push_back(animId);
    stc.animRefs.push_back(animRef);
    SortLookup(stc);
    // The state is the same id list read a second way; a container that
    // names one keeps it in step.
    if (stc.animationStateIndex < out.animationStates.size()) {
        out.animationStates[stc.animationStateIndex].animIds = stc.animIds;
    }
}

SubTrack MergeColorAlpha(const SubTrack* color, const SubTrack* alpha, const Vector3f& restColor,
                         f32 restAlpha) {
    SubTrack merged;
    const bool stepped = (color == nullptr || color->interp == Interpolation::Step) &&
                         (alpha == nullptr || alpha->interp == Interpolation::Step);
    merged.interp = stepped ? Interpolation::Step : Interpolation::Linear;
    std::vector<f32> times;
    if (color != nullptr) {
        times.insert(times.end(), color->times.begin(), color->times.end());
    }
    if (alpha != nullptr) {
        times.insert(times.end(), alpha->times.begin(), alpha->times.end());
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    for (const f32 time : times) {
        f32 rgb[4] = {restColor.x, restColor.y, restColor.z, 0.0f};
        f32 a[4] = {restAlpha, 0.0f, 0.0f, 0.0f};
        if (color != nullptr && !color->times.empty()) {
            SampleAt(*color, 3, time, rgb);
        }
        if (alpha != nullptr && !alpha->times.empty()) {
            SampleAt(*alpha, 1, time, a);
        }
        const f32 value[4] = {rgb[0], rgb[1], rgb[2], a[0]};
        const u8* bytes = reinterpret_cast<const u8*>(value);
        merged.times.push_back(time);
        merged.values.insert(merged.values.end(), bytes, bytes + sizeof(value));
    }
    return merged;
}

namespace {

template <class T>
void Note(std::vector<u32*>& out, m3::AnimRef<T>& ref) {
    out.push_back(&ref.animId);
}

void NoteLayer(std::vector<u32*>& out, std::optional<m3::TextureLayer>& layer) {
    if (!layer.has_value()) {
        return;
    }
    Note(out, layer->color);
    Note(out, layer->rgbMultiply);
    Note(out, layer->rgbAdd);
    Note(out, layer->aviPlay);
    Note(out, layer->aviRestart);
    Note(out, layer->currentFrame);
    Note(out, layer->uvOffset);
    Note(out, layer->uvAngle);
    Note(out, layer->uvTiling);
    Note(out, layer->wOffset);
    Note(out, layer->wTiling);
    Note(out, layer->mapAlpha);
    Note(out, layer->triplanarOffset);
    Note(out, layer->triplanarScale);
}

/// A copy of the block @p word names, appended to its own typed array; the
/// copy's word, or `kInvalidIndex`.
u32 CopyBlock(m3::SubTrackContainer& stc, u32 word) {
    const u32 slot = word >> 16;
    const std::size_t block = word & 0xFFFFu;
    const auto copy = [&](auto& blocks) -> u32 {
        if (block >= blocks.size() || blocks.size() >= 0xFFFFu) {
            return kInvalidIndex;
        }
        auto keys = blocks[block];
        blocks.push_back(std::move(keys));
        return (slot << 16) | static_cast<u32>(blocks.size() - 1);
    };
    switch (slot) {
    case 0:
        return copy(stc.sdev);
    case 1:
        return copy(stc.sd2v);
    case 2:
        return copy(stc.sd3v);
    case 3:
        return copy(stc.sd4q);
    case 4:
        return copy(stc.sdcc);
    case 5:
        return copy(stc.sdr3);
    case 6:
        return copy(stc.sdu8);
    case 7:
        return copy(stc.sds6);
    case 8:
        return copy(stc.sdu6);
    case 9:
        return copy(stc.sds3);
    case 10:
        return copy(stc.sdu3);
    case 11:
        return copy(stc.sdfg);
    case 12:
        return copy(stc.sdmb);
    default:
        return kInvalidIndex;
    }
}

} // namespace

std::vector<u32*> KeyableAnimIds(m3::Model& out) {
    std::vector<u32*> ids;
    for (m3::Bone& bone : out.bones) {
        Note(ids, bone.position);
        Note(ids, bone.rotation);
        Note(ids, bone.scale);
        Note(ids, bone.visibility);
    }
    for (m3::Light& light : out.lights) {
        Note(ids, light.diffuseColor);
        Note(ids, light.intensityMultiplier);
        Note(ids, light.specularColor);
        Note(ids, light.specularMultiplier);
        Note(ids, light.decay);
        Note(ids, light.attenuationStart);
        Note(ids, light.hotSpot);
        Note(ids, light.falloff);
    }
    for (m3::StandardMaterial& mat : out.standardMaterials) {
        Note(ids, mat.parallaxHeight);
        Note(ids, mat.motionBlurAmount);
        for (std::optional<m3::TextureLayer>* layer :
             {&mat.diffuseLayer, &mat.decalLayer, &mat.specularLayer, &mat.glossLayer,
              &mat.emissiveLayer1, &mat.emissiveLayer2, &mat.environmentLayer,
              &mat.environmentMaskLayer, &mat.alphaLayer1, &mat.alphaLayer2, &mat.normalLayer,
              &mat.heightLayer, &mat.lightMapLayer, &mat.ambientOcclusionLayer}) {
            NoteLayer(ids, *layer);
        }
    }
    for (m3::ParticleEmitter& p : out.particleEmitters) {
        for (m3::AnimRef<f32>* ref :
             {&p.initialSpeed, &p.initialSpeedRandom, &p.initialYaw, &p.initialPitch,
              &p.initialHorizontal, &p.initialVertical, &p.lifetime, &p.lifetimeRandom,
              &p.emissionRate, &p.outerRadius, &p.innerRadius}) {
            Note(ids, *ref);
        }
        for (m3::AnimRef<Vector3f>* ref : {&p.sizeAnimation, &p.rotationAnimation, &p.shapeOuter,
                                           &p.shapeInner, &p.sizeRandomAnimation,
                                           &p.rotationRandomAnimation}) {
            Note(ids, *ref);
        }
        for (m3::AnimRef<m3::ColorBGRA>* ref : {&p.colorStart, &p.colorMid, &p.colorEnd,
                                                &p.colorStartRandom, &p.colorMidRandom,
                                                &p.colorEndRandom}) {
            Note(ids, *ref);
        }
        Note(ids, p.squirtAmount);
    }
    for (m3::RibbonEmitter& r : out.ribbonEmitters) {
        for (m3::AnimRef<f32>* ref :
             {&r.initialSpeed, &r.initialSpeedRandom, &r.initialYaw, &r.initialPitch,
              &r.initialHorizontal, &r.initialVertical, &r.lifetime, &r.lifetimeRandom,
              &r.maxLength}) {
            Note(ids, *ref);
        }
        Note(ids, r.sizeAnimation);
        Note(ids, r.rotationAnimation);
        Note(ids, r.colorStart);
        Note(ids, r.colorMid);
        Note(ids, r.colorEnd);
        Note(ids, r.active);
    }
    return ids;
}

void UnshareAnimIds(m3::Model& out) {
    // Where each driven id is played: (container, block word) per entry.
    std::map<u32, std::vector<std::pair<u32, u32>>> driven;
    std::set<u32> taken;
    u32 highest = 0;
    for (u32 s = 0; s < out.subTrackCollections.size(); ++s) {
        const m3::SubTrackContainer& stc = out.subTrackCollections[s];
        const std::size_t count = (std::min)(stc.animIds.size(), stc.animRefs.size());
        for (std::size_t k = 0; k < count; ++k) {
            driven[stc.animIds[k]].emplace_back(s, stc.animRefs[k]);
            taken.insert(stc.animIds[k]);
            highest = (std::max)(highest, stc.animIds[k]);
        }
    }
    if (driven.empty()) {
        return;
    }
    const std::vector<u32*> ids = KeyableAnimIds(out);
    for (const u32* id : ids) {
        taken.insert(*id);
    }
    u32 cursor = highest;
    const auto fresh = [&] {
        do {
            ++cursor;
        } while (cursor == 0 || cursor == 0xFFFFFFFFu || taken.count(cursor) != 0);
        taken.insert(cursor);
        return cursor;
    };
    std::set<u32> claimed;
    for (u32* id : ids) {
        const auto played = driven.find(*id);
        if (played == driven.end() || claimed.insert(*id).second) {
            continue;
        }
        const u32 own = fresh();
        for (const auto& [stc, word] : played->second) {
            const u32 copy = CopyBlock(out.subTrackCollections[stc], word);
            if (copy != kInvalidIndex) {
                AddToContainer(out, stc, own, copy);
            }
        }
        *id = own;
    }
}

} // namespace m3_sink
} // namespace wem
} // namespace models
} // namespace whiteout
