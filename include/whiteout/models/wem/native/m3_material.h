// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3_material.h
 * @brief `native::M3Material` — the M3 block that unions the generated bodies.
 *
 * The eleven bodies in `m3_native.h` are mirrors and are generated. This is not:
 * a discriminated union over them, plus the two things a MADD material leaves
 * behind, is a decision about how WEM stores M3 — and §15.2's line is that
 * nothing WEM-semantic is generated from a format header.
 *
 * ### Why MADD is kept twice
 *
 * `MADD` is not a curiosity to normalise away. At load the SC2 renderer converts
 * every StandardMaterial, DisplacementMaterial and ReflectionMaterial in the
 * model into one, rewrites the MaterialMap, and consumes only that — so on any
 * v30+ file MADD *is* the material. It also carries data the standard form has
 * nowhere to put: `blendMode` lives in `unknown124`, and the environment op in
 * `EnvioControl`. But the runtime also restores a StandardMaterial from it, and
 * that restored form is what a renderer wants to look at.
 *
 * So the block keeps both, and `authoredDataDriven` says which one the file held.
 * Dropping either direction loses something real.
 *
 * ### The variant order is not the on-disk numbering
 *
 * `M3MaterialKind` is the shipped enum, 1..12, with `Hair` at 8. HAI_ is defunct
 * — the parser header says "always null" and the corpus agrees — so the variant
 * has no alternative for it and a non-null `hairMaterials` array diagnoses rather
 * than parsing. The two sequences therefore cannot be the same number, which is
 * why `BodyIndexFor` exists here and does not in `common.h`, where they are.
 */

#include <cstddef>
#include <optional>
#include <variant>

#include <whiteout/models/wem/chunk_traits.h>
#include <whiteout/models/wem/native/m3_native.h>

namespace whiteout {
namespace models {
namespace wem {
namespace native {

/// The M3 material bodies WEM stores, in variant order. `Hair` is absent on
/// purpose (see the file comment).
using M3Body = std::variant<M3Standard, M3Displacement, M3Composite, M3Terrain, M3Volume, M3Creep,
                            M3VolumeNoise, M3Stb, M3Reflection, M3LensFlare, M3DataDriven>;

/// Alternative index for @p kind, or `kM3NoBody` for `Hair` and for any value
/// a future MODL version puts on disk.
constexpr std::size_t kM3NoBody = static_cast<std::size_t>(-1);

constexpr std::size_t BodyIndexFor(M3MaterialKind kind) {
    switch (kind) {
    case M3MaterialKind::Standard:
        return 0;
    case M3MaterialKind::Displacement:
        return 1;
    case M3MaterialKind::Composite:
        return 2;
    case M3MaterialKind::Terrain:
        return 3;
    case M3MaterialKind::Volume:
        return 4;
    case M3MaterialKind::Creep:
        return 5;
    case M3MaterialKind::VolumeNoise:
        return 6;
    case M3MaterialKind::SplatTerrainBake:
        return 7;
    case M3MaterialKind::Reflection:
        return 8;
    case M3MaterialKind::LensFlare:
        return 9;
    case M3MaterialKind::DataDriven:
        return 10;
    default:
        return kM3NoBody;
    }
}

/// The inverse. Total, because a variant always holds something.
constexpr M3MaterialKind KindForBodyIndex(std::size_t index) {
    switch (index) {
    case 1:
        return M3MaterialKind::Displacement;
    case 2:
        return M3MaterialKind::Composite;
    case 3:
        return M3MaterialKind::Terrain;
    case 4:
        return M3MaterialKind::Volume;
    case 5:
        return M3MaterialKind::Creep;
    case 6:
        return M3MaterialKind::VolumeNoise;
    case 7:
        return M3MaterialKind::SplatTerrainBake;
    case 8:
        return M3MaterialKind::Reflection;
    case 9:
        return M3MaterialKind::LensFlare;
    case 10:
        return M3MaterialKind::DataDriven;
    default:
        return M3MaterialKind::Standard;
    }
}

/// The 12-type union, kept as a union, shared by `Sc2` and `Heroes` (§7.3).
struct M3Material {
    /// MODL version the record was read from, 23..30+. The same field means
    /// different things across that range, so a converter cannot work without it.
    u32 sourceVersion = 0;

    /// What the file's `MaterialMap` said. Kept separately from `body.index()`
    /// because they disagree by construction — see the file comment.
    M3MaterialKind kind = M3MaterialKind::Standard;

    /// True when `MADD` is what the file actually held, rather than a typed body
    /// the loader would have converted into one.
    bool authoredDataDriven = false;

    M3Body body = M3Standard{};

    /// The decoded `MADD` property dictionary. `MADD` stores it as an opaque
    /// blob; WEM keeps the decode so a consumer never re-runs it, and so a
    /// property whose name hash we cannot resolve still round-trips.
    std::optional<M3DataDrivenProperties> dataDrivenProperties;

    /// The StandardMaterial the runtime restores from a `MADD`. Lossy in the
    /// forward direction, which is why the authored blob above is kept too.
    std::optional<M3Standard> restoredStandard;

    /// The variant alternative currently held. Prefer this over `kind` when
    /// reading `body`; `kind` is what the file claimed.
    std::size_t bodyIndex() const {
        return body.index();
    }

    bool holdsDataDriven() const {
        return std::holds_alternative<M3DataDriven>(body);
    }

    template <class V>
    void reflect(V& v) {
        v.field("sourceVersion", sourceVersion);
        v.field("kind", kind);
        v.field("authoredDataDriven", authoredDataDriven);
        // `body` is the union and `kind` above is what the FILE claimed; the
        // two disagree by construction, so the body rides its own
        // discriminator and `kind` is preserved as the separate datum it is.
        const M3MaterialKind bodyKind =
            KindForBodyIndex(VariantKind<std::size_t>(v, "bodyIndex", body));
        switch (bodyKind) {
        case M3MaterialKind::Standard:
            v.field("standard", VariantAs<M3Standard>(body));
            break;
        case M3MaterialKind::Displacement:
            v.field("displacement", VariantAs<M3Displacement>(body));
            break;
        case M3MaterialKind::Composite:
            v.field("composite", VariantAs<M3Composite>(body));
            break;
        case M3MaterialKind::Terrain:
            v.field("terrain", VariantAs<M3Terrain>(body));
            break;
        case M3MaterialKind::Volume:
            v.field("volume", VariantAs<M3Volume>(body));
            break;
        case M3MaterialKind::Creep:
            v.field("creep", VariantAs<M3Creep>(body));
            break;
        case M3MaterialKind::VolumeNoise:
            v.field("volumeNoise", VariantAs<M3VolumeNoise>(body));
            break;
        case M3MaterialKind::SplatTerrainBake:
            v.field("splatTerrainBake", VariantAs<M3Stb>(body));
            break;
        case M3MaterialKind::Reflection:
            v.field("reflection", VariantAs<M3Reflection>(body));
            break;
        case M3MaterialKind::LensFlare:
            v.field("lensFlare", VariantAs<M3LensFlare>(body));
            break;
        case M3MaterialKind::DataDriven:
            v.field("dataDriven", VariantAs<M3DataDriven>(body));
            break;
        case M3MaterialKind::Hair:
            // `HAI_` is defunct -- the parser header says "always null" and the
            // corpus agrees -- so the union has no alternative for it and
            // `KindForBodyIndex` never returns it. Named here only because the
            // switch is exhaustive.
            break;
        }
        v.optional("dataDrivenProperties", dataDrivenProperties);
        v.optional("restoredStandard", restoredStandard);
    }
};

} // namespace native

template <>
struct ChunkTagTraits<native::M3Material> {
    static constexpr u32 value = kTag("NM3_");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

} // namespace wem
} // namespace models
} // namespace whiteout
