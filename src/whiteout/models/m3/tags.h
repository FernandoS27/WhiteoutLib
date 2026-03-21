// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <string>

#include <whiteout/common_types.h>

namespace whiteout::m3 {

// ============================================================================
// Tag Utilities
// ============================================================================

/**
 * @brief Create a FourCC tag from a 4-character string literal (big-endian storage)
 *
 * M3 tags are stored as 4 ASCII bytes in reverse order when read as a little-endian
 * uint32. This function creates the canonical tag value for comparison.
 *
 * @tparam N Size of the string literal (must be 5 including null terminator)
 * @param str 4-character string literal
 * @return u32 Tag value
 */
template <std::size_t N>
constexpr u32 makeTag(const char (&str)[N]) {
    static_assert(N == 5, "Tag must be exactly 4 characters (plus null terminator)");
    return static_cast<u32>(static_cast<u8>(str[0]) << 24) |
           (static_cast<u32>(static_cast<u8>(str[1])) << 16) |
           (static_cast<u32>(static_cast<u8>(str[2])) << 8) |
           (static_cast<u32>(static_cast<u8>(str[3])) << 0);
}

/// Convert a FourCC tag value back to a human-readable 4-character string.
constexpr std::string tagToString(u32 tag) {
    std::array<char, 5> chars = {
        static_cast<char>((tag >> 24) & 0xFF), static_cast<char>((tag >> 16) & 0xFF),
        static_cast<char>((tag >> 8) & 0xFF), static_cast<char>((tag >> 0) & 0xFF), '\0'};
    return std::string(chars.data(), 4);
}

// ============================================================================
// Known Tags (stored in reversed/canonical form for comparison)
// ============================================================================

// Header
constexpr u32 TAG_MD34 = makeTag("MD34"); ///< Release file magic (MD34)
constexpr u32 TAG_MD33 = makeTag("MD33"); ///< Beta file magic (MD33)

// Primitive data chunks
constexpr u32 TAG_CHAR = makeTag("CHAR"); ///< Null-terminated ASCII string (1 byte per element)
constexpr u32 TAG_U8 = makeTag("U8__");   ///< Unsigned 8-bit (vertex blobs, weights)
constexpr u32 TAG_U16 = makeTag("U16_");  ///< Unsigned 16-bit (indices, bone lookups)
constexpr u32 TAG_U32 = makeTag("U32_");  ///< Unsigned 32-bit (animation IDs, flags)
constexpr u32 TAG_U64 = makeTag("U64_");  ///< Unsigned 64-bit
constexpr u32 TAG_I16 = makeTag("I16_");  ///< Signed 16-bit
constexpr u32 TAG_I32 = makeTag("I32_");  ///< Signed 32-bit (frame timestamps)
constexpr u32 TAG_VEC2 = makeTag("VEC2"); ///< Vector2f (8 bytes)
constexpr u32 TAG_VEC3 = makeTag("VEC3"); ///< Vector3f (12 bytes)
constexpr u32 TAG_VEC4 = makeTag("VEC4"); ///< Vector4f (16 bytes)
constexpr u32 TAG_QUAT = makeTag("QUAT"); ///< Quaternion (16 bytes)
constexpr u32 TAG_REAL = makeTag("REAL"); ///< f32 scalar (4 bytes)
constexpr u32 TAG_FLAG = makeTag("FLAG"); ///< Flag bitfield (4 bytes)
constexpr u32 TAG_BNDS = makeTag("BNDS"); ///< Extent / bounding volume (28 bytes)
constexpr u32 TAG_COL = makeTag("\0COL"); ///< ColorBGRA (4 bytes)
constexpr u32 TAG_MT32 = makeTag("MT32"); ///< Physics mesh triangle indices (7 x u32, 28 bytes)
constexpr u32 TAG_MT16 = makeTag("MT16"); ///< Physics mesh triangle indices (7 x u16, 14 bytes)

// Model root
constexpr u32 TAG_MODL = makeTag("MODL"); ///< Model root chunk (all Ref<T> fields)

// Animation system
constexpr u32 TAG_SEQS = makeTag("SEQS"); ///< Animation sequences (frame ranges, bounds)
constexpr u32 TAG_STC = makeTag("STC_");  ///< Sub-track containers (keyframe data refs)
constexpr u32 TAG_STG = makeTag("STG_");  ///< Animation groups (name + STC indices)
constexpr u32 TAG_STS = makeTag("STS_");  ///< Animation states (animId lookup)
constexpr u32 TAG_BSET = makeTag("BSET"); ///< Bone animation sets

// Animation data blocks (keyframe storage — one tag per value type)
constexpr u32 TAG_SDEV = makeTag("SDEV"); ///< Slot 0: f32 keyframes
constexpr u32 TAG_SD2V = makeTag("SD2V"); ///< Slot 1: Vector2f keyframes
constexpr u32 TAG_SD3V = makeTag("SD3V"); ///< Slot 2: Vector3f keyframes
constexpr u32 TAG_SD4Q = makeTag("SD4Q"); ///< Slot 3: Quaternion keyframes
constexpr u32 TAG_SDCC = makeTag("SDCC"); ///< Slot 4: ColorBGRA keyframes
constexpr u32 TAG_SDR3 = makeTag("SDR3"); ///< Slot 5: f32 keyframes (rotation)
constexpr u32 TAG_SDU8 = makeTag("SDU8"); ///< Slot 6: u8 keyframes
constexpr u32 TAG_SDS6 = makeTag("SDS6"); ///< Slot 7: i16 keyframes
constexpr u32 TAG_SDU6 = makeTag("SDU6"); ///< Slot 8: u16 keyframes
constexpr u32 TAG_SDS3 = makeTag("SDS3"); ///< Slot 9: i32 keyframes
constexpr u32 TAG_SDU3 = makeTag("SDU3"); ///< Slot 10: u32 keyframes
constexpr u32 TAG_SDFG = makeTag("SDFG"); ///< Slot 11: Flag keyframes
constexpr u32 TAG_SDMB = makeTag("SDMB"); ///< Slot 12: Extent keyframes

// Skeleton & mesh
constexpr u32 TAG_BONE = makeTag("BONE"); ///< Skeleton bones (v1, 160 bytes)
constexpr u32 TAG_DIV = makeTag("DIV_");  ///< Mesh divisions (indices, regions, batches)
constexpr u32 TAG_REGN = makeTag("REGN"); ///< Submesh regions (vertex/index ranges)
constexpr u32 TAG_BAT = makeTag("BAT_");  ///< Draw call batches (region → material)
constexpr u32 TAG_MSEC = makeTag("MSEC"); ///< Mesh section bounding shapes
constexpr u32 TAG_IREF = makeTag("IREF"); ///< Inverse bind-pose matrices (one per bone)
constexpr u32 TAG_ATT = makeTag("ATT_");  ///< Attachment points (named bone locations)

// Materials
constexpr u32 TAG_MATM = makeTag("MATM"); ///< Material map (type + index into material arrays)
constexpr u32 TAG_MAT = makeTag("MAT_"); ///< Standard material (diffuse, specular, emissive layers)
constexpr u32 TAG_LAYR = makeTag("LAYR"); ///< Texture layer (UV animation, tint, fresnel)
constexpr u32 TAG_DIS = makeTag("DIS_");  ///< Displacement material
constexpr u32 TAG_CMP = makeTag("CMP_");  ///< Composite material (blended sub-materials)
constexpr u32 TAG_CMS = makeTag("CMS_");  ///< Composite material section
constexpr u32 TAG_TER = makeTag("TER_");  ///< Terrain material
constexpr u32 TAG_VOL = makeTag("VOL_");  ///< Volume material (uniform density fog)
constexpr u32 TAG_HAI = makeTag("HAI_");  ///< Hair material (defunct — always null)
constexpr u32 TAG_VON = makeTag("VON_");  ///< Volume noise material (noisy density fog)
constexpr u32 TAG_CREP = makeTag("CREP"); ///< Creep material (SC2 Zerg creep)
constexpr u32 TAG_STBM = makeTag("STBM"); ///< Splat terrain bake material
constexpr u32 TAG_REF = makeTag("REF_");  ///< Reflection material
constexpr u32 TAG_LFLR = makeTag("LFLR"); ///< Lens flare material
constexpr u32 TAG_MADD = makeTag("MADD"); ///< Buffer material / material add data (v30+)
constexpr u32 TAG_SCHR = makeTag("SCHR"); ///< String reference array (Ref<CHAR>, 12 bytes)

// Lights, cameras, events
constexpr u32 TAG_LITE = makeTag("LITE"); ///< Light (omni, spot, or directional)
constexpr u32 TAG_CAM = makeTag("CAM_");  ///< Camera (FOV, DOF, clip planes)
constexpr u32 TAG_EVNT = makeTag("EVNT"); ///< Event (sound triggers, effects)

// Particle systems
constexpr u32 TAG_PAR = makeTag("PAR_");  ///< Particle emitter (emission, physics, collision)
constexpr u32 TAG_PARC = makeTag("PARC"); ///< Particle emitter copy (overrides emission rate)
constexpr u32 TAG_RIB = makeTag("RIB_");  ///< Ribbon emitter (connected trail strips)
constexpr u32 TAG_SRIB = makeTag("SRIB"); ///< Spline ribbon sub-structure
constexpr u32 TAG_PROJ = makeTag("PROJ"); ///< Projector (projected textures / decals)
constexpr u32 TAG_FOR = makeTag("FOR_");  ///< Force (radial, wind, explosion)
constexpr u32 TAG_WRP = makeTag("WRP_");  ///< Warp (spatial distortion)

// Physics
constexpr u32 TAG_PHRB = makeTag("PHRB"); ///< Rigid body (collision shape + dynamics)
constexpr u32 TAG_PHSH = makeTag("PHSH"); ///< Physics shape (box, sphere, capsule, hull, mesh)
constexpr u32 TAG_PHYJ = makeTag("PHYJ"); ///< Physics joint (hinge, cone, etc.)
constexpr u32 TAG_PHCL = makeTag("PHCL"); ///< Cloth physics simulation (v28+)
constexpr u32 TAG_PHCC = makeTag("PHCC"); ///< Cloth collider (capsule shape)
constexpr u32 TAG_PHAC = makeTag("PHAC"); ///< Cloth proxy (vertex mapping)
constexpr u32 TAG_PHCT = makeTag("PHCT"); ///< Physics constraint (rigid body linkage)

// Miscellaneous
constexpr u32 TAG_SSGS = makeTag("SSGS"); ///< Hit test shape (box, sphere, capsule, mesh)
constexpr u32 TAG_ATVL = makeTag("ATVL"); ///< Attachment volume (hit test + dual bone refs)
constexpr u32 TAG_TRGD = makeTag("TRGD"); ///< Trigger data
constexpr u32 TAG_PATU = makeTag("PATU"); ///< Turret behavior (yaw/pitch limits)
constexpr u32 TAG_BBSC = makeTag("BBSC"); ///< Billboard behavior
constexpr u32 TAG_IKJT = makeTag("IKJT"); ///< IK joint (raycast-based foot planting)
constexpr u32 TAG_IK2J = makeTag("IK2J"); ///< Two-joint IK solver
constexpr u32 TAG_IKCC = makeTag("IKCC"); ///< CCD IK solver (v24+)
constexpr u32 TAG_PAOB = makeTag("PAOB"); ///< One-bone IK solver
constexpr u32 TAG_SHBX = makeTag("SHBX"); ///< Shadow box (64-byte matrix)
constexpr u32 TAG_DMMT = makeTag("DMMT"); ///< Physics mesh triangle (winged-edge, 28 bytes)
constexpr u32 TAG_DMME = makeTag("DMME"); ///< Physics mesh edge (winged-edge, 20 bytes)
constexpr u32 TAG_DMMN = makeTag("DMMN"); ///< Physics mesh normal (8 or 12 bytes)
constexpr u32 TAG_DMSE = makeTag("DMSE"); ///< Convex hull half-edge (4 bytes)
constexpr u32 TAG_VVOL = makeTag("VVOL"); ///< View volume (visibility culling)
constexpr u32 TAG_LFSB = makeTag("LFSB"); ///< Sub-flare entry (lens flare element)
constexpr u32 TAG_SVC2 = makeTag("SVC2"); ///< Static Vector2f channel (AnimRef<Vector2f>)
constexpr u32 TAG_SVC3 = makeTag("SVC3"); ///< Static Vector3f channel (AnimRef<Vector3f>)
constexpr u32 TAG_SS32 = makeTag("SS32"); ///< Static i32 channel
constexpr u32 TAG_SU32 = makeTag("SU32"); ///< Static u32 channel
constexpr u32 TAG_SR32 = makeTag("SR32"); ///< Static f32 channel (AnimRef<f32>)
constexpr u32 TAG_TMD = makeTag("TMD_");  ///< Trailing model (defunct)

} // namespace whiteout::m3
