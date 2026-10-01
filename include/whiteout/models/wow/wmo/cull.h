#pragma once

// ============================================================================
// 12.1's view cull for WMO placements (`CWorldViewCull`): which groups a camera
// sees, through the portals when it stands inside, and which doodads those
// groups draw. Everything is in world yards, as the client's is: planes are
// unit and the tolerances are its own. WMO_RENDER_RE.md part 14.
// ============================================================================

#include <array>
#include <cfloat>
#include <span>
#include <vector>

#include "parser.h"
#include "runtime.h"
#include "types.h"

namespace whiteout {
namespace models {
namespace wow {
namespace wmo {

/// A `C44Matrix` as the cull applies it: row vectors,
/// `p' = x·r0 + y·r1 + z·r2 + w·r3`.
struct Matrix4 {
    std::array<std::array<f32, 4>, 4> m{{{1.0f, 0.0f, 0.0f, 0.0f},
                                         {0.0f, 1.0f, 0.0f, 0.0f},
                                         {0.0f, 0.0f, 1.0f, 0.0f},
                                         {0.0f, 0.0f, 0.0f, 1.0f}}};
};

Vector3f transformPoint(const Vector3f& p, const Matrix4& m);

/// The view's eight world corners (`CWorldView_ComputeFrustumCorners`): the
/// near quad, then the far one, each at NDC (−1,−1), (−1,+1), (+1,+1), (+1,−1).
using FrustumCorners = std::array<Vector3f, 8>;

/// `CFrustum`: six unit planes, a point inside one when `n·p + d >= 0`, in
/// `CFrustum_FromCorners`'s order: top, bottom, left, right, far, near.
struct CullFrustum {
    std::array<Plane, 6> planes{};
};

/// A screen rectangle in NDC [−1, 1] or in [0, 1]; empty while max <= min.
struct CullRect {
    f32 minX = FLT_MAX;
    f32 minY = FLT_MAX;
    f32 maxX = -FLT_MAX;
    f32 maxY = -FLT_MAX;

    bool empty() const {
        return maxX <= minX || maxY <= minY;
    }
};

/// `CFrustum_FromCorners` 0x1436832F0.
CullFrustum frustumFromCorners(const FrustumCorners& corners);
/// `CFrustum_SubCornersFromRect` 0x141B8D310: @p corners narrowed to
/// @p rect, in [0, 1] (x along corner 0 → 3, y along 0 → 1).
FrustumCorners subCorners(const FrustumCorners& corners, const CullRect& rect);
/// `CFrustum_TestAABB` 0x143683750: @p box is not wholly outside a plane by
/// more than 7/360 yd.
bool frustumTouches(const CullFrustum& frustum, const Box& box);
/// `CFrustum_TestAABBMask` 0x143683C90: frustumTouches, and in @p inside the
/// planes @p box is wholly inside (bit i for plane i).
bool frustumTouches(const CullFrustum& frustum, const Box& box, u32& inside);
/// `CFrustum_TestAABBMasked` 0x143683A00: frustumTouches over @p mask's
/// planes only.
bool frustumTouchesMasked(const CullFrustum& frustum, const Box& box, u32 mask);
/// `sub_143682F80`: the world box of @p box under @p m (Arvo's method, the
/// exact box of its eight corners).
Box transformBox(const Box& box, const Matrix4& m);

/// One portal's record for a placement (`def+400`, 36 B), kept from frame to
/// frame: worked out on its first visit in a frame and reused after it.
struct PortalRecord {
    u32 frame = 0xFFFFFFFFu;
    u32 flags = 0; ///< 0x10 clipped away, 0x20 the eye is on it, 0x200 closed
    CullRect rect; ///< NDC
};

/// The camera as the cull holds it.
struct CullView {
    FrustumCorners corners{};
    Matrix4 worldToClip; ///< x/w and y/w are NDC
    Vector3f eye{0.0f, 0.0f, 0.0f};
};

/// One placement as the cull reads it.
struct CullPlacement {
    const Model* model = nullptr;
    Matrix4 localToWorld;
    Matrix4 worldToLocal;
    /// Per group, `CMapObjDefGroup+76`: its MOGI box in the world, grown by
    /// its doodads' boxes.
    std::span<const Box> groupBoxes;
    /// Per group, the LOD level it draws; a group with no file there is a
    /// placeholder, never drawn and never walked into.
    std::span<const u32> currentLod;
    /// `def+168` 0x40 / 0x100: the far proxy's group, drawn alone.
    std::optional<u32> proxyGroup;
    /// `def+464`: portals the placement has turned off (doors), by MOPT index.
    std::array<u32, 8> disabledPortals{};
    /// Per MOPT, persistent.
    std::vector<PortalRecord>* portals = nullptr;
};

/// A group the frame draws (`VisGroupInst`, `MapObjRender_AddVisGroupInst`
/// 0x141B85D20).
struct VisibleGroup {
    u32 group = 0;
    CullRect rect; ///< [0, 1]: the union of the rects that reached it
    u32 mask = 0;  ///< the planes its box straddles; 0 skips its doodads' tests
    bool doodads = false;
};

struct CullResult {
    std::vector<VisibleGroup> groups;
    /// What of the outside shows ([0, 1]): the whole screen from outside;
    /// from inside, what the interior pass saw through its exterior portals.
    CullRect exterior;
};

/// One frame of 12.1's cull for @p placement (`CWorldView_Cull` then
/// `CWorldCull_Run`). With @p inside, the camera stands in this WMO: its groups
/// walk the portals with the whole screen, and the exterior pass runs in what
/// they saw of the outside. Without it, the exterior pass runs in @p exterior,
/// the camera WMO's result (the whole screen when there is none); an empty one
/// draws nothing outdoors. @p frame keys the portal records.
CullResult cullGroups(const CullPlacement& placement, const CullView& view, const CameraLocation* inside,
                      u32 frame, const CullRect& exterior = CullRect{0.0f, 0.0f, 1.0f, 1.0f});

/// The doodads' distance and size fade (`MapChunk_UpdateDoodadFades`
/// 0x141B3B9B0), at 12.1's defaults: lodObjectCullSize 15 (k = 0.15),
/// lodObjectCullDist 30, and the horizon plus 33.333332 yd.
struct DoodadFadeRule {
    f32 k = 0.15f;
    f32 near = 30.0f;
    f32 far = 800.0f + 33.333332f;
};

/// The fade a doodad heads for: 0 when its box's centre is past the horizon
/// by more than its size, or past `near + 3/k` of its size, else 0xFF00.
/// @p size is the box's largest edge.
u16 doodadFadeTarget(const Box& box, f32 size, const Vector3f& eye, const DoodadFadeRule& rule);

/// @p current moved toward @p target by 256 a millisecond over @p elapsedMs.
u16 stepDoodadFade(u16 current, u16 target, u32 elapsedMs);

} // namespace wmo
} // namespace wow
} // namespace models
} // namespace whiteout
