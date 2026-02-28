#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <array>
#include <map>
#include <memory>
#include <span>

#include "../common/common_types.h"

namespace m2 {

using ::Extent;

template<typename T>
struct AnimationKey {
    u32 timestamp = 0;
    T value;
};

template<typename T>
struct SplineKey {
    u32 timestamp = 0;
    T value;
    T inTangent;
    T outTangent;
};

struct AnimationTrackBase {
    u16 interpolationType = 0;
    u16 globalSequenceId = 0xFFFF;
    std::vector<std::vector<u32>> timestamps;
};

template<typename T>
struct AnimationTrack : public AnimationTrackBase {
    std::vector<std::vector<T>> values;
};

constexpr u16 M2_VERSION_VANILLA = 256;
constexpr u16 M2_VERSION_BC = 260;
constexpr u16 M2_VERSION_WOTLK = 264;
constexpr u16 M2_VERSION_CATA = 265;
constexpr u16 M2_VERSION_MOP = 272;
constexpr u16 M2_VERSION_WOD = 272;
constexpr u16 M2_VERSION_LEGION = 272;
constexpr u16 M2_VERSION_BFA = 273;
constexpr u16 M2_VERSION_SL = 274;


template<std::size_t N>
constexpr u32 makeTag(const char (&str)[N]) {
    static_assert(N == 5, "Tag must be exactly 4 characters (plus null terminator)");
    return static_cast<u32>(static_cast<u8>(str[0])) |
           (static_cast<u32>(static_cast<u8>(str[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(str[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(str[3])) << 24);
}

constexpr u32 MD20_TAG = makeTag("MD20");
constexpr u32 MD21_TAG = makeTag("MD21");

constexpr u32 PFID_TAG = makeTag("PFID");
constexpr u32 SFID_TAG = makeTag("SFID");
constexpr u32 AFID_TAG = makeTag("AFID");
constexpr u32 BFID_TAG = makeTag("BFID");
constexpr u32 TXAC_TAG = makeTag("TXAC");
constexpr u32 EXPT_TAG = makeTag("EXPT");
constexpr u32 EXP2_TAG = makeTag("EXP2");
constexpr u32 PABC_TAG = makeTag("PABC");
constexpr u32 PADC_TAG = makeTag("PADC");
constexpr u32 PSBC_TAG = makeTag("PSBC");
constexpr u32 PEDC_TAG = makeTag("PEDC");
constexpr u32 SKID_TAG = makeTag("SKID");
constexpr u32 TXID_TAG = makeTag("TXID");
constexpr u32 LDV1_TAG = makeTag("LDV1");
constexpr u32 RPID_TAG = makeTag("RPID");
constexpr u32 GPID_TAG = makeTag("GPID");
constexpr u32 WFV1_TAG = makeTag("WFV1");
constexpr u32 WFV2_TAG = makeTag("WFV2");
constexpr u32 WFV3_TAG = makeTag("WFV3");
constexpr u32 PFDC_TAG = makeTag("PFDC");
constexpr u32 EDGF_TAG = makeTag("EDGF");
constexpr u32 NERF_TAG = makeTag("NERF");
constexpr u32 DETL_TAG = makeTag("DETL");
constexpr u32 DBOC_TAG = makeTag("DBOC");
constexpr u32 AFRA_TAG = makeTag("AFRA");
constexpr u32 PCOL_TAG = makeTag("PCOL");
constexpr u32 DPIV_TAG = makeTag("DPIV");
constexpr u32 TEXL_TAG = makeTag("TEXL");
constexpr u32 PGD1_TAG = makeTag("PGD1");

} // namespace m2
