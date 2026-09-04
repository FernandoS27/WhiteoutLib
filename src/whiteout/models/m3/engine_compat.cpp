// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Engine support divergence between StarCraft II and Heroes of the Storm.
// The four divergent chunks and the per-file (not per-chunk) enforcement are
// documented in M3_FILE_FORMAT_SPECIFICATION.md §18.

#include <whiteout/models/m3/engine_compat.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace whiteout {
namespace m3 {

namespace {

constexpr u32 NO_DATA_DRIVEN_LINK = 0xFFFFFFFFu;

std::string count_of(std::size_t n, const char* singular, const char* plural) {
    return std::to_string(n) + ' ' + (n == 1 ? singular : plural);
}

std::string materialLabel(const DataDrivenMaterial& material, std::size_t index) {
    if (!material.materialName.empty()) {
        return material.materialName;
    }
    return "MADD[" + std::to_string(index) + ']';
}

} // namespace

EngineSupport checkEngineSupport(const Model& model) {
    EngineSupport out;

    if (model.getVersion() > SC2_MAX_MODEL_VERSION) {
        out.heroesOnlyReasons.push_back("MODL is v" + std::to_string(model.getVersion()) +
                                        "; StarCraft II caps MODL at v" +
                                        std::to_string(SC2_MAX_MODEL_VERSION));
    }
    if (!model.dataDrivenMaterials.empty()) {
        out.heroesOnlyReasons.push_back(
            count_of(model.dataDrivenMaterials.size(), "MADD record", "MADD records") +
            "; StarCraft II has no MADD support");
    }

    const std::size_t dataDrivenMaps = static_cast<std::size_t>(std::count_if(
        model.materialMaps.begin(), model.materialMaps.end(),
        [](const MaterialMap& map) { return map.materialType == MaterialType::DataDriven; }));
    if (dataDrivenMaps > 0) {
        out.heroesOnlyReasons.push_back(
            count_of(dataDrivenMaps, "material map entry", "material map entries") +
            " of type DataDriven");
    }

    const std::size_t newReflections = static_cast<std::size_t>(
        std::count_if(model.reflectionMaterials.begin(), model.reflectionMaterials.end(),
                      [](const ReflectionMaterial& material) {
                          return material.getVersion() > SC2_MAX_REFLECTION_MATERIAL_VERSION;
                      }));
    if (newReflections > 0) {
        out.heroesOnlyReasons.push_back(
            count_of(newReflections, "REF_ record", "REF_ records") + " above v" +
            std::to_string(SC2_MAX_REFLECTION_MATERIAL_VERSION) + "; StarCraft II caps REF_ at v" +
            std::to_string(SC2_MAX_REFLECTION_MATERIAL_VERSION));
    }

    const std::size_t newStandards = static_cast<std::size_t>(
        std::count_if(model.standardMaterials.begin(), model.standardMaterials.end(),
                      [](const StandardMaterial& material) {
                          return material.getVersion() > HOTS_MAX_STANDARD_MATERIAL_VERSION;
                      }));
    if (newStandards > 0) {
        out.starcraft2OnlyReasons.push_back(count_of(newStandards, "MAT_ record", "MAT_ records") +
                                            " above v" +
                                            std::to_string(HOTS_MAX_STANDARD_MATERIAL_VERSION) +
                                            "; Heroes of the Storm caps MAT_ at v" +
                                            std::to_string(HOTS_MAX_STANDARD_MATERIAL_VERSION));
    }

    out.starcraft2 = out.heroesOnlyReasons.empty();
    out.heroesOfTheStorm = out.starcraft2OnlyReasons.empty();
    return out;
}

bool isHeroesOnly(const Model& model) {
    const EngineSupport support = checkEngineSupport(model);
    return !support.starcraft2 && support.heroesOfTheStorm;
}

EngineConversion toStarCraft2(const Model& model, const StarCraft2ConversionOptions& options) {
    EngineConversion out;
    out.model = model;

    if (checkEngineSupport(model).starcraft2) {
        out.converted = true;
        return out;
    }

    Model& dst = out.model;

    // The writer takes a chunk's version from its first element, so appended
    // materials have to agree with the ones already there.
    i32 standardVersion = HOTS_MAX_STANDARD_MATERIAL_VERSION;
    if (!dst.standardMaterials.empty()) {
        standardVersion = dst.standardMaterials.front().getVersion();
        if (standardVersion < HOTS_MAX_STANDARD_MATERIAL_VERSION) {
            out.lossy.push_back("converted materials written at the model's MAT_ v" +
                                std::to_string(standardVersion) +
                                "; normal-blend layers and blend factors are dropped");
        }
    }

    std::vector<u32> standardIndexOf(dst.dataDrivenMaterials.size(), NO_DATA_DRIVEN_LINK);
    std::vector<std::string> refused;

    for (std::size_t i = 0; i < dst.dataDrivenMaterials.size(); ++i) {
        const DataDrivenMaterial& source = dst.dataDrivenMaterials[i];
        StandardMaterialConversion conversion = source.toStandardMaterial();
        if (!conversion.converted && options.approximate) {
            conversion = source.approximateStandardMaterial();
        }

        const std::string label = materialLabel(source, i);
        if (!conversion.converted) {
            refused.push_back(label + " (" + conversion.blocker + ')');
            continue;
        }
        for (const std::string& reason : conversion.lossy) {
            out.lossy.push_back(label + ": " + reason);
        }

        conversion.material.forceVersion(standardVersion);
        standardIndexOf[i] = static_cast<u32>(dst.standardMaterials.size());
        dst.standardMaterials.push_back(std::move(conversion.material));
    }

    if (!refused.empty()) {
        out.blocker =
            count_of(refused.size(), "data-driven material has", "data-driven materials have") +
            " no StandardMaterial equivalent: ";
        for (std::size_t i = 0; i < refused.size(); ++i) {
            out.blocker += (i == 0 ? "" : ", ") + refused[i];
        }
        return out;
    }

    for (MaterialMap& map : dst.materialMaps) {
        if (map.materialType != MaterialType::DataDriven) {
            continue;
        }
        if (map.materialIndex >= standardIndexOf.size()) {
            out.blocker = "material map references MADD index " +
                          std::to_string(map.materialIndex) + " but the model holds " +
                          count_of(standardIndexOf.size(), "record", "records");
            return out;
        }
        map.materialType = MaterialType::Standard;
        map.materialIndex = standardIndexOf[map.materialIndex];
    }
    dst.dataDrivenMaterials.clear();

    for (ReflectionMaterial& reflection : dst.reflectionMaterials) {
        if (reflection.getVersion() <= SC2_MAX_REFLECTION_MATERIAL_VERSION) {
            continue;
        }
        if (reflection.unknown2 != NO_DATA_DRIVEN_LINK) {
            out.lossy.push_back((reflection.name.empty() ? std::string("REF_") : reflection.name) +
                                ": dropped its data-driven material link");
        }
        reflection.unknown2 = NO_DATA_DRIVEN_LINK;
        reflection.forceVersion(SC2_MAX_REFLECTION_MATERIAL_VERSION);
    }

    if (dst.getVersion() > SC2_MAX_MODEL_VERSION) {
        dst.forceVersion(SC2_MAX_MODEL_VERSION);
    }

    out.converted = true;
    return out;
}

} // namespace m3
} // namespace whiteout
