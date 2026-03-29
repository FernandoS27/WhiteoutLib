// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/converters.h>

#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// FormatConverter — default implementations
// ============================================================================

ConvertResult FormatConverter::importFromBytes(std::span<const u8>) const {
    ConvertResult result;
    result.issues.push_back("Import from bytes not supported for format: " + formatId());
    return result;
}

ExportResult FormatConverter::exportToBytes(const Model&, u32) const {
    ExportResult result;
    result.issues.push_back("Export to bytes not supported for format: " + formatId());
    return result;
}

// ============================================================================
// ConverterRegistry
// ============================================================================

struct ConverterRegistry::Impl {
    std::vector<std::shared_ptr<FormatConverter>> converters;
    std::unordered_map<std::string, size_t> idToIndex;
};

ConverterRegistry& ConverterRegistry::instance() {
    static ConverterRegistry registry;
    return registry;
}

ConverterRegistry::ConverterRegistry() : pImpl(std::make_unique<Impl>()) {
    // Register built-in converters
    registerConverter(std::make_shared<MdxConverter>());
    registerConverter(std::make_shared<M2Converter>());
    registerConverter(std::make_shared<M3Converter>());
}

ConverterRegistry::~ConverterRegistry() = default;

void ConverterRegistry::registerConverter(std::shared_ptr<FormatConverter> converter) {
    if (!converter)
        return;
    auto id = converter->formatId();
    auto it = pImpl->idToIndex.find(id);
    if (it != pImpl->idToIndex.end()) {
        pImpl->converters[it->second] = std::move(converter);
    } else {
        pImpl->idToIndex[id] = pImpl->converters.size();
        pImpl->converters.push_back(std::move(converter));
    }
}

const FormatConverter* ConverterRegistry::find(const std::string& formatId) const {
    auto it = pImpl->idToIndex.find(formatId);
    return (it != pImpl->idToIndex.end()) ? pImpl->converters[it->second].get() : nullptr;
}

std::vector<const FormatConverter*> ConverterRegistry::all() const {
    std::vector<const FormatConverter*> result;
    result.reserve(pImpl->converters.size());
    for (const auto& c : pImpl->converters) {
        result.push_back(c.get());
    }
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
