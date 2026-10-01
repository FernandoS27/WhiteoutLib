// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/interfaces.h>
#include <whiteout/models/wow/wmo/parser.h>
#include <whiteout/models/wow/wmo/runtime.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace whiteout {
namespace models {
namespace wow {
namespace wmo {

namespace {

// Root chunks (CMapObj_ReadRootChunks 0x141A7EC80).
constexpr u32 MVER = makeTag("MVER");
constexpr u32 MOHD = makeTag("MOHD");
constexpr u32 MOTX = makeTag("MOTX");
constexpr u32 MOGN = makeTag("MOGN");
constexpr u32 MOSB = makeTag("MOSB");
constexpr u32 MOSI = makeTag("MOSI");
constexpr u32 MOGI = makeTag("MOGI");
constexpr u32 MGI2 = makeTag("MGI2");
constexpr u32 MOPV = makeTag("MOPV");
constexpr u32 MOPT = makeTag("MOPT");
constexpr u32 MOPR = makeTag("MOPR");
constexpr u32 MOPE = makeTag("MOPE");
constexpr u32 MOVV = makeTag("MOVV");
constexpr u32 MOVB = makeTag("MOVB");
constexpr u32 MOLT = makeTag("MOLT");
constexpr u32 MOLV = makeTag("MOLV");
constexpr u32 MNLD = makeTag("MNLD");
constexpr u32 MODS = makeTag("MODS");
constexpr u32 MODN = makeTag("MODN");
constexpr u32 MODI = makeTag("MODI");
constexpr u32 MODD = makeTag("MODD");
constexpr u32 MDDI = makeTag("MDDI");
constexpr u32 MDDL = makeTag("MDDL");
constexpr u32 MFOG = makeTag("MFOG");
constexpr u32 MFOB = makeTag("MFOB");
constexpr u32 MFED = makeTag("MFED");
constexpr u32 MPVD = makeTag("MPVD");
constexpr u32 MAVG = makeTag("MAVG");
constexpr u32 MAVD = makeTag("MAVD");
constexpr u32 MBVD = makeTag("MBVD");
constexpr u32 MCVP = makeTag("MCVP");
constexpr u32 MOMT = makeTag("MOMT");
constexpr u32 MOUV = makeTag("MOUV");
constexpr u32 MOMX = makeTag("MOMX");
constexpr u32 MOM3 = makeTag("MOM3");
constexpr u32 MOTN = makeTag("MOTN");
constexpr u32 GFID = makeTag("GFID");

// Group chunks (CMapObjGroup_ReadChunks 0x141A826D0).
constexpr u32 MOGP = makeTag("MOGP");
constexpr u32 MOGX = makeTag("MOGX");
constexpr u32 MOPY = makeTag("MOPY");
constexpr u32 MPY2 = makeTag("MPY2");
constexpr u32 MOVI = makeTag("MOVI");
constexpr u32 MOVX = makeTag("MOVX");
constexpr u32 MOVT = makeTag("MOVT");
constexpr u32 MONR = makeTag("MONR");
constexpr u32 MOTV = makeTag("MOTV");
constexpr u32 MOCV = makeTag("MOCV");
constexpr u32 MOC2 = makeTag("MOC2");
constexpr u32 MOTA = makeTag("MOTA");
constexpr u32 MOBA = makeTag("MOBA");
constexpr u32 MOBS = makeTag("MOBS");
constexpr u32 MOPB = makeTag("MOPB");
constexpr u32 MOLR = makeTag("MOLR");
constexpr u32 MOLP = makeTag("MOLP");
constexpr u32 MOP2 = makeTag("MOP2");
constexpr u32 MOLS = makeTag("MOLS");
constexpr u32 MOS2 = makeTag("MOS2");
constexpr u32 MNLR = makeTag("MNLR");
constexpr u32 MODR = makeTag("MODR");
constexpr u32 MPVR = makeTag("MPVR");
constexpr u32 MAVR = makeTag("MAVR");
constexpr u32 MBVR = makeTag("MBVR");
constexpr u32 MFVR = makeTag("MFVR");
constexpr u32 MFBR = makeTag("MFBR");
constexpr u32 MLSP = makeTag("MLSP");
constexpr u32 MLSS = makeTag("MLSS");
constexpr u32 MLSK = makeTag("MLSK");
constexpr u32 MLSO = makeTag("MLSO");
constexpr u32 MDAL = makeTag("MDAL");
constexpr u32 MOPL = makeTag("MOPL");
constexpr u32 MOQG = makeTag("MOQG");
constexpr u32 MLIQ = makeTag("MLIQ");
constexpr u32 MOBN = makeTag("MOBN");
constexpr u32 MOBR = makeTag("MOBR");
// Group chunks 12.1 no longer reads; skipped without complaint.
constexpr u32 MORI = makeTag("MORI");
constexpr u32 MORB = makeTag("MORB");

// Where a group file's sub-chunks start: the client reads the MOGP header at
// +0x14 and walks from +0x58 to the end of the file, ignoring MOGP's own size.
constexpr std::size_t kGroupHeaderOffset = 0x14;
constexpr std::size_t kGroupChunksOffset = kGroupHeaderOffset + sizeof(GroupHeader);

u32 read32(std::span<const u8> data, std::size_t at) {
    u32 v = 0;
    std::memcpy(&v, data.data() + at, 4);
    return v;
}

std::string tagName(u32 tag) {
    const char c[4] = {static_cast<char>(tag >> 24), static_cast<char>(tag >> 16),
                       static_cast<char>(tag >> 8), static_cast<char>(tag)};
    return std::string(c, 4);
}

} // namespace

class Parser::Impl {
public:
    std::vector<std::string> issues;

    void report(std::string message) {
        issues.push_back(std::move(message));
    }

    // A flat run of fixed-size records: the count is the size over the stride,
    // and a remainder is reported and dropped.
    template <typename T>
    std::vector<T> records(u32 tag, std::span<const u8> payload) {
        if (payload.size() % sizeof(T) != 0) {
            report(tagName(tag) + ": " + std::to_string(payload.size()) +
                   " bytes is not a whole number of " + std::to_string(sizeof(T)) +
                   "-byte records");
        }
        std::vector<T> out(payload.size() / sizeof(T));
        if (!out.empty())
            std::memcpy(static_cast<void*>(out.data()), payload.data(), out.size() * sizeof(T));
        return out;
    }

    std::optional<Root> parseRoot(std::span<const u8> data);
    std::optional<Group> parseGroup(std::span<const u8> data);
    void readLiquid(std::span<const u8> payload, Group& group);
};

std::optional<Root> Parser::Impl::parseRoot(std::span<const u8> data) {
    Root root;
    bool sawHeader = false;
    std::size_t at = 0;
    while (at + 8 <= data.size()) {
        const u32 tag = read32(data, at);
        const u32 size = read32(data, at + 4);
        if (size > data.size() - at - 8) {
            report("root: " + tagName(tag) + " (" + std::to_string(size) +
                   " bytes) runs past the end of the file");
            return std::nullopt;
        }
        const std::span<const u8> payload = data.subspan(at + 8, size);
        at += 8 + static_cast<std::size_t>(size);

        switch (tag) {
        case MVER:
            if (size >= 4) {
                root.version = read32(payload, 0);
                if (root.version != kVersion) {
                    report("root: version " + std::to_string(root.version) + ", 12.1 reads 17");
                    return std::nullopt;
                }
            }
            break;
        case MOHD:
            // The client reads 64 bytes whatever the size says.
            std::memcpy(static_cast<void*>(&root.header), payload.data(),
                        std::min<std::size_t>(size, sizeof(Header)));
            if (size < sizeof(Header))
                report("root: MOHD is " + std::to_string(size) + " bytes, short of 64");
            sawHeader = true;
            break;
        case MOTX:
            root.textureNames.emplace(payload.begin(), payload.end());
            break;
        case MOGN:
            root.groupNames.assign(payload.begin(), payload.end());
            break;
        case MOSB: {
            std::size_t n = 0;
            while (n < payload.size() && payload[n] != 0)
                ++n;
            root.skyboxName.emplace(reinterpret_cast<const char*>(payload.data()), n);
            break;
        }
        case MOSI:
            if (size >= 4)
                root.skyboxFileId = read32(payload, 0);
            break;
        case MOGI:
            root.groups = records<GroupInfo>(tag, payload);
            root.groupCountFromMgi2 = false;
            break;
        case MGI2:
            root.groups2 = records<GroupInfo2>(tag, payload);
            root.groupCountFromMgi2 = true;
            break;
        case MOPV:
            root.portalVertices = records<Vector3f>(tag, payload);
            break;
        case MOPT:
            root.portals = records<Portal>(tag, payload);
            break;
        case MOPR:
            root.portalRefs = records<PortalRef>(tag, payload);
            break;
        case MOPE:
            root.portalExtras = records<PortalExtra>(tag, payload);
            break;
        case MOVV:
            root.visibleBlockVertices = records<Vector3f>(tag, payload);
            break;
        case MOVB:
            root.visibleBlocks = records<VisibleBlock>(tag, payload);
            break;
        case MOLT:
            root.lights = records<Light>(tag, payload);
            break;
        case MOLV:
            root.lightExtensions = records<LightExtension>(tag, payload);
            break;
        case MNLD:
            root.newLights = records<NewLight>(tag, payload);
            break;
        case MODS:
            root.doodadSets = records<DoodadSet>(tag, payload);
            break;
        case MODN:
            root.doodadNames.assign(payload.begin(), payload.end());
            break;
        case MODI:
            root.doodadFileIds = records<u32>(tag, payload);
            break;
        case MODD:
            root.doodadDefs = records<DoodadDef>(tag, payload);
            break;
        case MDDI:
            root.doodadColorMultipliers = records<f32>(tag, payload);
            break;
        case MDDL:
            root.detailDoodads.assign(payload.begin(), payload.end());
            break;
        case MFOG:
            root.fogs = records<Fog>(tag, payload);
            break;
        case MFOB:
            root.fogBoxes = records<FogBox>(tag, payload);
            break;
        case MFED:
            root.fogExtras = records<FogExtra>(tag, payload);
            break;
        case MPVD:
            // Taken only as a whole number of records; otherwise the client
            // drops the chunk.
            if (size % sizeof(ParticulateVolume) == 0)
                root.particulateVolumes = records<ParticulateVolume>(tag, payload);
            else
                report("root: MPVD is not a whole number of 4272-byte records; ignored");
            break;
        case MAVG:
            root.globalAmbients = records<AmbientVolume>(tag, payload);
            break;
        case MAVD:
            root.ambientVolumes = records<AmbientVolume>(tag, payload);
            break;
        case MBVD:
            root.ambientBoxes = records<AmbientBox>(tag, payload);
            break;
        case MCVP:
            root.convexVolumePlanes = records<Plane>(tag, payload);
            break;
        case MOMT:
            root.materials = records<Material>(tag, payload);
            break;
        case MOUV:
            root.uvAnimations = records<MaterialUvAnimation>(tag, payload);
            break;
        case MOMX:
            root.materialExtensions = records<MaterialExtension>(tag, payload);
            break;
        case MOM3:
            root.m3Materials.assign(payload.begin(), payload.end());
            break;
        case MOTN:
            root.m3TextureNames = records<std::array<u32, 9>>(tag, payload);
            break;
        case GFID:
            root.groupFileIds = records<u32>(tag, payload);
            break;
        default:
            report("root: unknown chunk " + tagName(tag) + " (" + std::to_string(size) + " bytes)");
            break;
        }
    }
    // The client's walk has to land on the end of the file exactly.
    if (at != data.size()) {
        report("root: " + std::to_string(data.size() - at) + " trailing bytes");
        return std::nullopt;
    }
    if (!sawHeader) {
        report("root: no MOHD");
        return std::nullopt;
    }
    return root;
}

void Parser::Impl::readLiquid(std::span<const u8> payload, Group& group) {
    // A 0x1E-byte header, so everything after it is unaligned.
    constexpr std::size_t kHeader = 0x1E;
    if (payload.size() < kHeader) {
        report("group: MLIQ is shorter than its header");
        return;
    }
    Liquid liquid;
    std::memcpy(&liquid.xVertices, payload.data() + 0x00, 4);
    std::memcpy(&liquid.yVertices, payload.data() + 0x04, 4);
    std::memcpy(&liquid.xTiles, payload.data() + 0x08, 4);
    std::memcpy(&liquid.yTiles, payload.data() + 0x0C, 4);
    std::memcpy(&liquid.corner, payload.data() + 0x10, 12);
    std::memcpy(&liquid.material, payload.data() + 0x1C, 2);
    const u64 vertexCount = static_cast<u64>(std::max(liquid.xVertices, 0)) *
                            static_cast<u64>(std::max(liquid.yVertices, 0));
    const u64 tileCount =
        static_cast<u64>(std::max(liquid.xTiles, 0)) * static_cast<u64>(std::max(liquid.yTiles, 0));
    const u64 need = kHeader + vertexCount * sizeof(LiquidVertex) + tileCount;
    if (need > payload.size()) {
        report("group: MLIQ declares " + std::to_string(vertexCount) + " vertices and " +
               std::to_string(tileCount) + " tiles, more than its " +
               std::to_string(payload.size()) + " bytes hold");
        return;
    }
    liquid.vertices.resize(static_cast<std::size_t>(vertexCount));
    if (vertexCount)
        std::memcpy(static_cast<void*>(liquid.vertices.data()), payload.data() + kHeader,
                    static_cast<std::size_t>(vertexCount) * sizeof(LiquidVertex));
    const std::size_t tilesAt = kHeader + static_cast<std::size_t>(vertexCount) * 8;
    liquid.tiles.assign(payload.begin() + tilesAt,
                        payload.begin() + tilesAt + static_cast<std::size_t>(tileCount));
    group.liquid = std::move(liquid);
}

std::optional<Group> Parser::Impl::parseGroup(std::span<const u8> data) {
    if (data.size() < kGroupChunksOffset) {
        report("group: " + std::to_string(data.size()) + " bytes cannot hold the MOGP header");
        return std::nullopt;
    }
    Group group;
    if (read32(data, 0) == MVER && read32(data, 4) >= 4)
        group.version = read32(data, 8);
    if (read32(data, 12) != MOGP)
        report("group: the chunk at +12 is " + tagName(read32(data, 12)) + ", not MOGP");
    std::memcpy(static_cast<void*>(&group.header), data.data() + kGroupHeaderOffset,
                sizeof(GroupHeader));

    std::span<const u8> tangentChunk;
    std::size_t tangentBatches = 0;
    std::size_t at = kGroupChunksOffset;
    while (at + 8 <= data.size()) {
        const u32 tag = read32(data, at);
        const u32 size = read32(data, at + 4);
        if (size > data.size() - at - 8) {
            report("group: " + tagName(tag) + " (" + std::to_string(size) +
                   " bytes) runs past the end of the file");
            break;
        }
        const std::span<const u8> payload = data.subspan(at + 8, size);
        at += 8 + static_cast<std::size_t>(size);

        switch (tag) {
        case MOGX:
            if (size >= 4)
                group.queryFaceStart = read32(payload, 0);
            if (size >= 5)
                group.detailDoodadKey = payload[4];
            break;
        case MOPY: {
            // The client widens the two-byte form into MPY2's.
            group.polys.resize(size / 2);
            for (std::size_t i = 0; i < group.polys.size(); ++i) {
                const u8 material = payload[i * 2 + 1];
                group.polys[i] =
                    Poly{payload[i * 2], static_cast<u16>(material == 0xFF ? 0xFFFF : material)};
            }
            group.polysFromMopy = true;
            break;
        }
        case MPY2:
            group.polys = records<Poly>(tag, payload);
            group.polysFromMopy = false;
            break;
        case MOVI: {
            const auto narrow = records<u16>(tag, payload);
            group.indices.assign(narrow.begin(), narrow.end());
            group.wideIndices = false;
            break;
        }
        case MOVX:
            group.indices = records<u32>(tag, payload);
            group.wideIndices = true;
            break;
        case MOVT:
            group.positions = records<Vector3f>(tag, payload);
            break;
        case MONR:
            group.normals = records<Vector3f>(tag, payload);
            break;
        case MOTV:
            group.uvSets.push_back(records<Vector2f>(tag, payload));
            break;
        case MOCV:
            group.vertexColors.push_back(records<Color>(tag, payload));
            break;
        case MOC2:
            group.vertexColors2 = records<Color>(tag, payload);
            break;
        case MOTA:
            // Indexed per batch, so it needs MOBA's count as of this chunk.
            tangentChunk = payload;
            tangentBatches = group.batches.size();
            break;
        case MOBA:
            group.batches = records<Batch>(tag, payload);
            break;
        case MOBS:
            group.shadowBatches = records<Batch>(tag, payload);
            break;
        case MOPB:
            group.prepassBatches = records<Batch>(tag, payload);
            break;
        case MOLR:
            group.lightRefs = records<u16>(tag, payload);
            break;
        case MOLP:
            group.pointLights = records<PointLight>(tag, payload);
            break;
        case MOP2:
            group.pointLightAnims = records<PointLightAnim>(tag, payload);
            break;
        case MOLS:
            group.spotLights = records<SpotLight>(tag, payload);
            break;
        case MOS2:
            group.spotLightAnims = records<SpotLightAnim>(tag, payload);
            break;
        case MNLR:
            group.newLightRefs = records<u16>(tag, payload);
            break;
        case MODR:
            group.doodadRefs = records<u16>(tag, payload);
            break;
        case MPVR:
            group.particulateRefs = records<u16>(tag, payload);
            break;
        case MAVR:
            group.ambientVolumeRefs = records<u16>(tag, payload);
            break;
        case MBVR:
            group.ambientBoxRefs = records<u16>(tag, payload);
            break;
        case MFVR:
            group.fogRefs = records<u16>(tag, payload);
            break;
        case MFBR:
            group.fogBoxRefs = records<u16>(tag, payload);
            break;
        case MLSP:
            group.pointLightSets = records<LightSetRange>(tag, payload);
            break;
        case MLSS:
            group.spotLightSets = records<LightSetRange>(tag, payload);
            break;
        case MLSK:
            group.pointAnimSets = records<LightSetRange>(tag, payload);
            break;
        case MLSO:
            group.spotAnimSets = records<LightSetRange>(tag, payload);
            break;
        case MDAL:
            if (size >= 4) {
                Color c;
                std::memcpy(&c, payload.data(), 4);
                group.ambientOverride = c;
            }
            break;
        case MOPL:
            group.terrainCutPlanes = records<Plane>(tag, payload);
            break;
        case MOQG:
            group.groundTypes = records<u32>(tag, payload);
            break;
        case MLIQ:
            readLiquid(payload, group);
            break;
        case MOBN:
            group.bspNodes = records<BspNode>(tag, payload);
            break;
        case MOBR:
            group.bspFaces = records<u16>(tag, payload);
            break;
        case MORI:
        case MORB:
            break;
        default:
            report("group: unknown chunk " + tagName(tag) + " (" + std::to_string(size) +
                   " bytes)");
            break;
        }
    }

    if (!tangentChunk.empty()) {
        Tangents t;
        const std::size_t indexBytes = tangentBatches * 2;
        if (indexBytes <= tangentChunk.size()) {
            t.firstTangent.resize(tangentBatches);
            if (tangentBatches)
                std::memcpy(t.firstTangent.data(), tangentChunk.data(), indexBytes);
            t.tangents.resize((tangentChunk.size() - indexBytes) / sizeof(Vector4f));
            if (!t.tangents.empty())
                std::memcpy(static_cast<void*>(t.tangents.data()), tangentChunk.data() + indexBytes,
                            t.tangents.size() * sizeof(Vector4f));
            group.tangents = std::move(t);
        } else {
            report("group: MOTA is shorter than its per-batch index");
        }
    }
    return group;
}

Parser::Parser() : pImpl(std::make_unique<Impl>()) {}

Parser::~Parser() = default;

std::optional<Root> Parser::parseRoot(std::span<const u8> data) {
    return pImpl->parseRoot(data);
}

std::optional<Group> Parser::parseGroup(std::span<const u8> data) {
    return pImpl->parseGroup(data);
}

std::optional<Model> Parser::parse(interfaces::CascFileSystem& fs, std::span<const u8> rootData) {
    pImpl->issues.clear();
    auto root = pImpl->parseRoot(rootData);
    if (!root)
        return std::nullopt;
    Model model;
    model.root = std::move(*root);
    const u32 count = model.root.groupCount();
    model.groups.resize(count);
    for (u32 g = 0; g < count; ++g) {
        const u32 fileId = model.root.groupFileId(g, 0);
        if (fileId == 0) {
            pImpl->report("group " + std::to_string(g) + ": GFID names no file");
            continue;
        }
        const std::vector<u8> bytes = fs.readFile(fileId);
        if (bytes.empty()) {
            pImpl->report("group " + std::to_string(g) + ": file " + std::to_string(fileId) +
                          " could not be read");
            continue;
        }
        model.groups[g] = pImpl->parseGroup(bytes);
    }
    return model;
}

std::optional<Model> Parser::parse(interfaces::VirtualPathFileSystem& fs,
                                   const std::string& rootPath) {
    pImpl->issues.clear();
    const std::vector<u8> rootData = fs.readFile(rootPath);
    if (rootData.empty()) {
        pImpl->report(rootPath + " could not be read");
        return std::nullopt;
    }
    auto root = pImpl->parseRoot(rootData);
    if (!root)
        return std::nullopt;
    Model model;
    model.root = std::move(*root);
    const u32 count = model.root.groupCount();
    model.groups.resize(count);
    for (u32 g = 0; g < count; ++g) {
        const std::string path = groupFilePath(rootPath, g, 0);
        const std::vector<u8> bytes = fs.readFile(path);
        if (bytes.empty()) {
            pImpl->report(path + " could not be read");
            continue;
        }
        model.groups[g] = pImpl->parseGroup(bytes);
    }
    return model;
}

namespace {

template <typename Read>
bool LoadLodWith(Model& model, u32 lod, Read read) {
    if (lod == 0 || lod > maxLodLevel(model.root))
        return false;
    if (model.lodGroups.size() < lod)
        model.lodGroups.resize(lod);
    auto& level = model.lodGroups[lod - 1];
    const u32 count = model.root.groupCount();
    level.assign(count, std::nullopt);
    for (u32 g = 0; g < count; ++g) {
        if (groupHasLod(model.root, g, lod))
            level[g] = read(g);
    }
    return true;
}

} // namespace

bool Parser::loadLod(Model& model, u32 lod, interfaces::CascFileSystem& fs) {
    return LoadLodWith(model, lod, [&](u32 g) -> std::optional<Group> {
        const u32 fileId = model.root.groupFileId(g, lod);
        if (fileId == 0)
            return std::nullopt;
        const std::vector<u8> bytes = fs.readFile(fileId);
        if (bytes.empty())
            return std::nullopt;
        return pImpl->parseGroup(bytes);
    });
}

bool Parser::loadLod(Model& model, u32 lod, interfaces::VirtualPathFileSystem& fs,
                     const std::string& rootPath) {
    return LoadLodWith(model, lod, [&](u32 g) -> std::optional<Group> {
        const std::vector<u8> bytes = fs.readFile(groupFilePath(rootPath, g, lod));
        if (bytes.empty())
            return std::nullopt;
        return pImpl->parseGroup(bytes);
    });
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

FileKind detectFileKind(std::span<const u8> data) {
    if (data.size() < 16 || read32(data, 0) != MVER)
        return FileKind::Unknown;
    const u64 next = 8 + static_cast<u64>(read32(data, 4));
    if (next + 4 > data.size())
        return FileKind::Unknown;
    const u32 second = read32(data, static_cast<std::size_t>(next));
    if (second == MOHD)
        return FileKind::Root;
    if (second == MOGP)
        return FileKind::Group;
    return FileKind::Unknown;
}

std::string groupFilePath(const std::string& rootPath, u32 group, u32 lod) {
    std::string stem = rootPath;
    if (stem.size() >= 4) {
        std::string ext = stem.substr(stem.size() - 4);
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".wmo")
            stem.resize(stem.size() - 4);
    }
    char suffix[32];
    if (lod == 0)
        std::snprintf(suffix, sizeof(suffix), "_%03u.wmo", group);
    else
        std::snprintf(suffix, sizeof(suffix), "_%03u_lod%u.wmo", group, lod);
    return stem + suffix;
}

const std::vector<Color>* Group::colorSet0() const {
    if (hasFlag(header.flags, GroupFlag::VertexColors) && !vertexColors.empty())
        return &vertexColors.front();
    return nullptr;
}

const std::vector<Color>* Group::colorSet1() const {
    const std::size_t first = colorSet0() ? 1 : 0;
    return vertexColors.size() > first ? &vertexColors.back() : nullptr;
}

std::string_view Root::nameAt(const std::vector<char>& block, i64 offset) {
    if (offset < 0 || static_cast<u64>(offset) >= block.size())
        return {};
    const char* begin = block.data() + offset;
    const char* end = block.data() + block.size();
    const char* stop = std::find(begin, end, '\0');
    return {begin, static_cast<std::size_t>(stop - begin)};
}

} // namespace wmo
} // namespace wow
} // namespace models
} // namespace whiteout
