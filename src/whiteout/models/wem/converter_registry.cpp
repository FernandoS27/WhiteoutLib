// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/converters.h"

#include <algorithm>
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
