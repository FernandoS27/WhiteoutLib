#pragma once

#include "structures/m2_base.h"
#include "structures/m2_chunks.h"

#include <optional>
#include <limits>

namespace m2 {

enum class M2Format : u32 {
    ClassicMD20 = 0,
    LegionMD21 = 1,

    Invalid = std::numeric_limits<u32>::max(),
};

struct M2File {
    M2Format format = M2Format::Invalid;
    MD20Header header;
    std::optional<M2PFIDChunk> pfid_chunk = std::nullopt;
    std::optional<M2SFIDChunk> sfid_chunk = std::nullopt;
    std::optional<M2AFIDChunk> afid_chunk = std::nullopt;
    std::optional<M2BFIDChunk> bfid_chunk = std::nullopt;
    std::optional<M2TXACChunk> txac_chunk = std::nullopt;
    std::optional<M2EXPTChunk> expt_chunk = std::nullopt;
    std::optional<M2EXP2Chunk> exp2_chunk = std::nullopt;
    std::optional<M2PABCChunk> pabc_chunk = std::nullopt;
    std::optional<M2PADCChunk> padc_chunk = std::nullopt;
    std::optional<M2PSBCChunk> psbc_chunk = std::nullopt;
    std::optional<M2PEDCChunk> pedc_chunk = std::nullopt;
    std::optional<M2SKIDChunk> skid_chunk = std::nullopt;
    std::optional<M2TXIDChunk> txid_chunk = std::nullopt;
    std::optional<M2LDV1Chunk> ldv1_chunk = std::nullopt;
    std::optional<M2RPIDChunk> rpid_chunk = std::nullopt;
    std::optional<M2GPIDChunk> gpid_chunk = std::nullopt;
    std::optional<M2WFV1Chunk> wfv1_chunk = std::nullopt;
    std::optional<M2WFV2Chunk> wfv2_chunk = std::nullopt;
    std::optional<M2PGD1Chunk> pgd1_chunk = std::nullopt;
    std::optional<M2WFV3Chunk> wfv3_chunk = std::nullopt;
    std::optional<M2PFDCChunk> pfdc_chunk = std::nullopt;
    std::optional<M2EDGFChunk> edgf_chunk = std::nullopt;
    std::optional<M2NERFChunk> nerf_chunk = std::nullopt;
    std::optional<M2DETLChunk> detl_chunk = std::nullopt;
    std::optional<M2DBOCChunk> dboc_chunk = std::nullopt;
    std::optional<M2AFRAChunk> afra_chunk = std::nullopt;
    std::optional<M2PCOLChunk> pcol_chunk = std::nullopt;
    std::optional<M2DPIVChunk> dpiv_chunk = std::nullopt;
    std::optional<M2TEXLChunk> texl_chunk = std::nullopt;
};

struct M2SkinFile {
    // placeholder;
};

struct M2AnimFile {
    // placeholder;
};

struct M2SkeletonFile {
    // placeholder;
};

struct M2FileSystem {
    M2File m2;
    std::vector<M2SkinFile> skins;
    std::vector<M2AnimFile> anims;
    std::vector<M2SkeletonFile> skeletons;
};

} // namespace m2
