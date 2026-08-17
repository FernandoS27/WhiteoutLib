
#pragma once

#include <whiteout/models/m2/structures/phys.h>

namespace whiteout {
namespace m2 {

/// `.phys` writes its chunk ids reversed — the ADT/WMO convention, not the
/// M2 one — so "PHYS" is the bytes `S`,`Y`,`H`,`P`. Reading the id as a
/// little-endian u32 and comparing against this gives the tag as spelled.
template <std::size_t N>
constexpr u32 makePhysTag(const char (&str)[N]) {
    static_assert(N == 5, "Tag must be exactly 4 characters (plus null terminator)");
    return static_cast<u32>(static_cast<u8>(str[3])) |
           (static_cast<u32>(static_cast<u8>(str[2])) << 8) |
           (static_cast<u32>(static_cast<u8>(str[1])) << 16) |
           (static_cast<u32>(static_cast<u8>(str[0])) << 24);
}

constexpr u32 PHYS_TAG = makePhysTag("PHYS");
constexpr u32 PHYT_TAG = makePhysTag("PHYT");
constexpr u32 BODY_TAG = makePhysTag("BODY");
constexpr u32 BDY2_TAG = makePhysTag("BDY2");
constexpr u32 BDY3_TAG = makePhysTag("BDY3");
constexpr u32 BDY4_TAG = makePhysTag("BDY4");
constexpr u32 SHAP_TAG = makePhysTag("SHAP");
constexpr u32 SHP2_TAG = makePhysTag("SHP2");
constexpr u32 BOXS_TAG = makePhysTag("BOXS");
constexpr u32 CAPS_TAG = makePhysTag("CAPS");
constexpr u32 SPHS_TAG = makePhysTag("SPHS");
constexpr u32 PLYT_TAG = makePhysTag("PLYT");
constexpr u32 JOIN_TAG = makePhysTag("JOIN");
constexpr u32 WELJ_TAG = makePhysTag("WELJ");
constexpr u32 WLJ2_TAG = makePhysTag("WLJ2");
constexpr u32 WLJ3_TAG = makePhysTag("WLJ3");
constexpr u32 SPHJ_TAG = makePhysTag("SPHJ");
constexpr u32 SHOJ_TAG = makePhysTag("SHOJ");
constexpr u32 SHJ2_TAG = makePhysTag("SHJ2");
constexpr u32 PRSJ_TAG = makePhysTag("PRSJ");
constexpr u32 PRS2_TAG = makePhysTag("PRS2");
constexpr u32 REVJ_TAG = makePhysTag("REVJ");
constexpr u32 REV2_TAG = makePhysTag("REV2");
constexpr u32 DSTJ_TAG = makePhysTag("DSTJ");
constexpr u32 PHYV_TAG = makePhysTag("PHYV");

/// Which on-disk record layout a chunk holds. Up to version 2 the layout
/// followed the file version under a fixed chunk name; from 2 on each layout
/// got its own name, and both spellings still turn up.
enum class PhysBodyLayout : u8 { Body, Body2, Body3, Body4 };
enum class PhysShapeLayout : u8 { Shape, Shape2 };
enum class PhysWeldLayout : u8 { Weld, Weld2, Weld3 };
enum class PhysShoulderLayout : u8 { Shoulder, ShoulderMotor, Shoulder2 };
/// REVJ/PRSJ against REV2/PRS2: the same record plus a motor spring.
enum class PhysMotorLayout : u8 { Base, Sprung };

constexpr u32 physBodyStride(PhysBodyLayout layout) {
    switch (layout) {
    case PhysBodyLayout::Body:
        return 0x1C;
    case PhysBodyLayout::Body2:
        return 0x20;
    case PhysBodyLayout::Body3:
        return 0x2C;
    case PhysBodyLayout::Body4:
        return 0x30;
    }
    return 0x30;
}

constexpr u32 physShapeStride(PhysShapeLayout layout) {
    return layout == PhysShapeLayout::Shape ? 0x14 : 0x20;
}

constexpr u32 physWeldStride(PhysWeldLayout layout) {
    switch (layout) {
    case PhysWeldLayout::Weld:
        return 0x68;
    case PhysWeldLayout::Weld2:
        return 0x70;
    case PhysWeldLayout::Weld3:
        return 0x74;
    }
    return 0x74;
}

constexpr u32 physShoulderStride(PhysShoulderLayout layout) {
    switch (layout) {
    case PhysShoulderLayout::Shoulder:
        return 0x6C;
    case PhysShoulderLayout::ShoulderMotor:
        return 0x74;
    case PhysShoulderLayout::Shoulder2:
        return 0x7C;
    }
    return 0x7C;
}

constexpr u32 physRevoluteStride(PhysMotorLayout layout) {
    return layout == PhysMotorLayout::Base ? 0x70 : 0x78;
}

constexpr u32 physPrismaticStride(PhysMotorLayout layout) {
    return layout == PhysMotorLayout::Base ? 0x78 : 0x80;
}

constexpr u32 PHYS_BOX_STRIDE = 0x3C;
constexpr u32 PHYS_CAPSULE_STRIDE = 0x1C;
constexpr u32 PHYS_SPHERE_STRIDE = 0x10;
constexpr u32 PHYS_JOIN_STRIDE = 0x10;
constexpr u32 PHYS_SPHERICAL_STRIDE = 0x1C;
constexpr u32 PHYS_DISTANCE_STRIDE = 0x1C;
constexpr u32 PHYS_TUNING_STRIDE = 0x18;
constexpr u32 PHYS_POLYTOPE_HEADER_STRIDE = 0x50;

/// The layouts a given file version writes. Versions 4 and 5 are identical on
/// the wire; version 6 is what every inline PFDC payload in the WoW corpus
/// carries, and it is the one that switched welds, shoulders, revolutes and
/// prismatics over to their `*2`/`*3` spellings.
constexpr PhysBodyLayout physBodyLayoutFor(u16 version) {
    if (version < 2)
        return PhysBodyLayout::Body;
    if (version == 2)
        return PhysBodyLayout::Body2;
    if (version == 3)
        return PhysBodyLayout::Body3;
    return PhysBodyLayout::Body4;
}

constexpr PhysShapeLayout physShapeLayoutFor(u16 version) {
    return version < 2 ? PhysShapeLayout::Shape : PhysShapeLayout::Shape2;
}

constexpr PhysWeldLayout physWeldLayoutFor(u16 version) {
    if (version < 2)
        return PhysWeldLayout::Weld;
    return version < 6 ? PhysWeldLayout::Weld2 : PhysWeldLayout::Weld3;
}

constexpr PhysShoulderLayout physShoulderLayoutFor(u16 version) {
    if (version < 2)
        return PhysShoulderLayout::Shoulder;
    return version < 6 ? PhysShoulderLayout::ShoulderMotor : PhysShoulderLayout::Shoulder2;
}

constexpr PhysMotorLayout physMotorLayoutFor(u16 version) {
    return version < 6 ? PhysMotorLayout::Base : PhysMotorLayout::Sprung;
}

} // namespace m2
} // namespace whiteout
