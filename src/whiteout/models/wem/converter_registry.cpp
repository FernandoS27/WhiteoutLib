// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/converters.h"
#include "whiteout/models/wem/d3_converter.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// FormatConverter defaults
// ============================================================================

Result<Document> FormatConverter::importFromBytes(std::span<const u8>) const {
    Result<Document> result;
    result.diagnostics.error(DiagCode::OperationUnsupported,
                             formatName() + " does not support import from bytes");
    return result;
}

Result<std::vector<u8>> FormatConverter::exportToBytes(const Document&, ProfileId, u32) const {
    Result<std::vector<u8>> result;
    result.diagnostics.error(DiagCode::OperationUnsupported,
                             formatName() + " does not support export");
    return result;
}

bool FormatConverter::checkExportProfile(const Document& document, ProfileId profile,
                                         Diagnostics& out) const {
    const std::span<const ProfileId> served = profiles();
    if (std::find(served.begin(), served.end(), profile) == served.end()) {
        out.error(DiagCode::ProfileNotCarried,
                  formatName() + " does not serve profile " + ToString(profile), {}, profile);
        return false;
    }
    if (!document.carries(profile)) {
        out.error(DiagCode::ProfileNotCarried,
                  "the document does not carry profile " + std::string(ToString(profile)), {},
                  profile);
        return false;
    }
    return true;
}

void FormatConverter::checkRigConvention(const Document& document, ProfileId profile,
                                        Diagnostics& out) const {
    const RigConvention want = Profile(profile).rig;
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        const Model& model = document.models[m];
        if (model.nodes.empty() || model.nodes.rig == want) {
            continue;
        }
        out.warn(DiagCode::RigConventionChanged,
                 std::string("model '") + model.name + "' holds a " +
                     ToString(model.nodes.rig) + " rig and " + ToString(profile) + " wants " +
                     ToString(want) + "; run RetargetSkeleton first or the node tracks mean "
                                      "the wrong thing",
                 ElementRef(ElementKind::Document, static_cast<u32>(m)), profile);
    }
}

void FormatConverter::checkNodeKinds(const Document& document, ProfileId profile,
                                     Diagnostics& out) const {
    std::array<u32, static_cast<std::size_t>(NodeKind::Count)> uncarried{};
    for (const Model& model : document.models) {
        for (const Node& node : model.nodes.nodes) {
            if (static_cast<u32>(node.kind) < uncarried.size() &&
                !CarriesNodeKind(profile, node.kind)) {
                ++uncarried[static_cast<std::size_t>(node.kind)];
            }
        }
    }
    for (std::size_t k = 0; k < uncarried.size(); ++k) {
        if (uncarried[k] == 0) {
            continue;
        }
        out.info(DiagCode::NodeKindNotCarried,
                 std::to_string(uncarried[k]) + " " + ToString(static_cast<NodeKind>(k)) +
                     " nodes: " + ToString(profile) +
                     " does not carry the system, so only their placement is exported",
                 ElementRef(ElementKind::Document, 0), profile);
    }
}

// ============================================================================
// ConverterRegistry
// ============================================================================

struct ConverterRegistry::Impl {
    std::vector<std::shared_ptr<FormatConverter>> converters;
};

void RegisterBuiltinConverters(ConverterRegistry& registry) {
    registry.registerConverter(std::make_shared<MdxConverter>());
    registry.registerConverter(std::make_shared<M2Converter>());
    registry.registerConverter(std::make_shared<M3Converter>());
    registry.registerConverter(std::make_shared<D3Converter>());
    // Registered last on purpose: `findForProfile` prefers earlier entries, and
    // glTF listing `Generic` must never shadow a game converter's answer.
    registry.registerConverter(std::make_shared<GltfConverter>());
}

ConverterRegistry::ConverterRegistry() : pImpl(std::make_unique<Impl>()) {
    RegisterBuiltinConverters(*this);
}

ConverterRegistry::~ConverterRegistry() = default;

ConverterRegistry& ConverterRegistry::instance() {
    static ConverterRegistry registry;
    return registry;
}

void ConverterRegistry::registerConverter(std::shared_ptr<FormatConverter> converter) {
    if (converter == nullptr) {
        return;
    }
    const std::string id = converter->formatId();
    for (auto& existing : pImpl->converters) {
        if (existing->formatId() == id) {
            existing = std::move(converter);
            return;
        }
    }
    pImpl->converters.push_back(std::move(converter));
}

const FormatConverter* ConverterRegistry::find(const std::string& formatId) const {
    for (const auto& converter : pImpl->converters) {
        if (converter->formatId() == formatId) {
            return converter.get();
        }
    }
    return nullptr;
}

const FormatConverter* ConverterRegistry::findForProfile(ProfileId profile) const {
    for (const auto& converter : pImpl->converters) {
        const std::span<const ProfileId> served = converter->profiles();
        if (std::find(served.begin(), served.end(), profile) != served.end()) {
            return converter.get();
        }
    }
    return nullptr;
}

std::vector<const FormatConverter*> ConverterRegistry::all() const {
    std::vector<const FormatConverter*> out;
    out.reserve(pImpl->converters.size());
    for (const auto& converter : pImpl->converters) {
        out.push_back(converter.get());
    }
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout
