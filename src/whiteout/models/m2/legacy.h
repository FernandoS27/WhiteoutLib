#pragma once

#include <vector>

#include <whiteout/models/m2/structures.h>
#include <whiteout/models/m2/types.h>

namespace whiteout {
namespace m2 {

// ≤TBC files hold every track's keys on one shared timeline; each sequence
// owns a [start, end] window of it. These helpers are the client's exact
// M2CompQuat component mapping — not snorm16, which lacks the ±1 shift across
// zero — so a vanilla float quaternion converts to the same raw values a
// TBC-era re-export would have stored.
inline u16 compressQuatComponent(f32 f) {
    if (f < -1.0f)
        f = -1.0f;
    if (f > 1.0f)
        f = 1.0f;
    i32 v = static_cast<i32>(f * 32767.0f);
    v += (f > 0.0f) ? -32768 : 32767;
    return static_cast<u16>(static_cast<i16>(v));
}

inline f32 decompressQuatComponent(u16 raw) {
    i32 const v = static_cast<i16>(raw);
    return static_cast<f32>(v < 0 ? v + 32768 : v - 32767) / 32767.0f;
}

inline CompatQuaternion compressQuat(const Vector4f& q) {
    return CompatQuaternion(compressQuatComponent(q.x), compressQuatComponent(q.y),
                            compressQuatComponent(q.z), compressQuatComponent(q.w));
}

inline Vector4f decompressQuat(const CompatQuaternion& q) {
    return Vector4f{decompressQuatComponent(q.x), decompressQuatComponent(q.y),
                    decompressQuatComponent(q.z), decompressQuatComponent(q.w)};
}

/// One sequence's [start, end] window on the ≤TBC global timeline.
struct LegacyWindow {
    u32 start = 0;
    u32 end = 0;
};

/// One sequence's slice of a legacy track's flat key array, as index pairs
/// into it — the "interpolation ranges" ≤TBC files carry.
struct LegacyRange {
    u32 first = 0;
    u32 last = 0;
};

/// Gap inserted between sequence windows when a modern model is laid out onto
/// the global timeline. Blizzard's own spacing varies per model and nothing
/// reads the gaps; this matches the value community converters settled on.
constexpr u32 LEGACY_TIMELINE_GAP = 3333;

inline std::vector<LegacyWindow> buildLegacyTimeline(const std::vector<Sequence>& sequences) {
    std::vector<LegacyWindow> windows;
    windows.reserve(sequences.size());
    u32 timeline = 0;
    for (const auto& seq : sequences) {
        timeline += LEGACY_TIMELINE_GAP;
        LegacyWindow w;
        w.start = timeline;
        timeline += seq.duration;
        w.end = timeline;
        windows.push_back(w);
    }
    return windows;
}

} // namespace m2
} // namespace whiteout
