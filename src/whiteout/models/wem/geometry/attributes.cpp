// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/attributes.h>

#include <cstring>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

struct ReservedRow {
    const char* name;
    Domain domain;
    AttrType type;
};

/// §5.4's reserved-name table, minus the two families, which are prefix rules.
constexpr ReservedRow kReserved[] = {
    {names::kPosition, Domain::Vertex, AttrType::F32x3},
    {names::kMergeGroup, Domain::Vertex, AttrType::U32},
    {names::kNormal, Domain::Halfedge, AttrType::F32x3},
    {names::kTangent, Domain::Halfedge, AttrType::F32x4},
    {names::kBinormal, Domain::Halfedge, AttrType::F32x3},
    {names::kCrease, Domain::Edge, AttrType::F32},
    {names::kSharp, Domain::Edge, AttrType::Bool},
    {names::kSeam, Domain::Edge, AttrType::Bool},
    {names::kSection, Domain::Face, AttrType::U32},
    {names::kSmoothGroup, Domain::Face, AttrType::U32},
};

/// True when @p name is @p prefix followed by one or more decimal digits.
bool isIndexedFamily(const std::string& name, const char* prefix) {
    const std::size_t prefixLength = std::strlen(prefix);
    if (name.size() <= prefixLength || name.compare(0, prefixLength, prefix) != 0) {
        return false;
    }
    for (std::size_t i = prefixLength; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

std::string indexedName(const char* prefix, u32 index) {
    std::string out = prefix;
    if (index == 0) {
        out.push_back('0');
        return out;
    }
    char digits[12];
    int length = 0;
    while (index != 0) {
        digits[length++] = static_cast<char>('0' + (index % 10));
        index /= 10;
    }
    while (length > 0) {
        out.push_back(digits[--length]);
    }
    return out;
}

} // namespace

// ============================================================================

const char* ToString(Domain domain) {
    switch (domain) {
    case Domain::Vertex:
        return "vertex";
    case Domain::Halfedge:
        return "halfedge";
    case Domain::Edge:
        return "edge";
    case Domain::Face:
        return "face";
    case Domain::Mesh:
        return "mesh";
    case Domain::Count:
        break;
    }
    return "invalid";
}

const char* ToString(AttrType type) {
    switch (type) {
    case AttrType::F32:
        return "f32";
    case AttrType::F32x2:
        return "f32x2";
    case AttrType::F32x3:
        return "f32x3";
    case AttrType::F32x4:
        return "f32x4";
    case AttrType::U8x4:
        return "u8x4";
    case AttrType::U16:
        return "u16";
    case AttrType::U32:
        return "u32";
    case AttrType::I32:
        return "i32";
    case AttrType::Quat:
        return "quat";
    case AttrType::Bool:
        return "bool";
    case AttrType::Count:
        break;
    }
    return "invalid";
}

u32 AttrTypeSize(AttrType type) {
    switch (type) {
    case AttrType::F32:
        return 4;
    case AttrType::F32x2:
        return 8;
    case AttrType::F32x3:
        return 12;
    case AttrType::F32x4:
        return 16;
    case AttrType::U8x4:
        return 4;
    case AttrType::U16:
        return 2;
    case AttrType::U32:
        return 4;
    case AttrType::I32:
        return 4;
    case AttrType::Quat:
        return 16;
    case AttrType::Bool:
        return 1;
    case AttrType::Count:
        break;
    }
    return 0;
}

u32 AttrTypeComponents(AttrType type) {
    switch (type) {
    case AttrType::F32:
    case AttrType::U16:
    case AttrType::U32:
    case AttrType::I32:
    case AttrType::Bool:
        return 1;
    case AttrType::F32x2:
        return 2;
    case AttrType::F32x3:
        return 3;
    case AttrType::F32x4:
    case AttrType::U8x4:
    case AttrType::Quat:
        return 4;
    case AttrType::Count:
        break;
    }
    return 0;
}

namespace names {

std::string uv(u32 index) {
    return indexedName("uv", index);
}

std::string color(u32 index) {
    return indexedName("color", index);
}

} // namespace names

ReservedLayer LookupReserved(const std::string& name) {
    for (const ReservedRow& row : kReserved) {
        if (name == row.name) {
            return ReservedLayer{row.domain, row.type};
        }
    }
    if (isIndexedFamily(name, "uv")) {
        return ReservedLayer{Domain::Halfedge, AttrType::F32x2};
    }
    if (isIndexedFamily(name, "color")) {
        return ReservedLayer{Domain::Halfedge, AttrType::U8x4};
    }
    return ReservedLayer{};
}

// ============================================================================

void AttributeSet::setDomainCount(Domain domain, u32 count) {
    domainCounts_[static_cast<std::size_t>(domain)] = count;
    for (AttrLayer& layer : layers_) {
        if (layer.domain == domain) {
            layer.data.resize(static_cast<std::size_t>(AttrTypeSize(layer.type)) * count, 0);
        }
    }
}

u32 AttributeSet::find(const std::string& name, Domain domain) const {
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        if (layers_[i].domain == domain && layers_[i].name == name) {
            return static_cast<u32>(i);
        }
    }
    return kInvalidId;
}

const AttrLayer* AttributeSet::layer(const std::string& name, Domain domain) const {
    const u32 index = find(name, domain);
    return index == kInvalidId ? nullptr : &layers_[index];
}

AttrLayer* AttributeSet::layer(const std::string& name, Domain domain) {
    const u32 index = find(name, domain);
    return index == kInvalidId ? nullptr : &layers_[index];
}

AttrLayer& AttributeSet::create(const std::string& name, Domain domain, AttrType type,
                                utils::AttributeEncoding storage) {
    const u32 existing = find(name, domain);
    if (existing != kInvalidId) {
        return layers_[existing];
    }

    // A reserved name keeps its documented type: the table is the contract every
    // converter and the render view read the layer through.
    const ReservedLayer reserved = LookupReserved(name);
    const AttrType effective =
        (reserved.reserved() && reserved.domain == domain) ? reserved.type : type;

    AttrLayer added;
    added.name = name;
    added.domain = domain;
    added.type = effective;
    added.storage = storage;
    added.data.assign(static_cast<std::size_t>(AttrTypeSize(effective)) * domainCount(domain), 0);
    layers_.push_back(std::move(added));
    return layers_.back();
}

bool AttributeSet::remove(const std::string& name, Domain domain) {
    const u32 index = find(name, domain);
    if (index == kInvalidId) {
        return false;
    }
    layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

u32 AttributeSet::appendElement(Domain domain) {
    const u32 index = domainCount(domain);
    setDomainCount(domain, index + 1);
    return index;
}

void AttributeSet::copyElement(Domain domain, u32 from, u32 to) {
    if (from == to) {
        return;
    }
    for (AttrLayer& layer : layers_) {
        if (layer.domain != domain) {
            continue;
        }
        const std::size_t stride = AttrTypeSize(layer.type);
        const std::size_t src = stride * from;
        const std::size_t dst = stride * to;
        if (src + stride > layer.data.size() || dst + stride > layer.data.size()) {
            continue;
        }
        std::memcpy(layer.data.data() + dst, layer.data.data() + src, stride);
    }
}

void AttributeSet::remapDomain(Domain domain, std::span<const u32> remap, u32 newCount) {
    for (AttrLayer& layer : layers_) {
        if (layer.domain != domain) {
            continue;
        }
        const std::size_t stride = AttrTypeSize(layer.type);
        std::vector<u8> rebuilt(stride * newCount, 0);
        const std::size_t limit = layer.data.size() / (stride == 0 ? 1 : stride);
        for (std::size_t old = 0; old < remap.size() && old < limit; ++old) {
            const u32 fresh = remap[old];
            if (fresh == kInvalidId || fresh >= newCount) {
                continue;
            }
            std::memcpy(rebuilt.data() + stride * fresh, layer.data.data() + stride * old, stride);
        }
        layer.data = std::move(rebuilt);
    }
    domainCounts_[static_cast<std::size_t>(domain)] = newCount;
}

void AttributeSet::clear() {
    layers_.clear();
    for (std::size_t i = 0; i < static_cast<std::size_t>(Domain::Count); ++i) {
        domainCounts_[i] = 0;
    }
    domainCounts_[static_cast<std::size_t>(Domain::Mesh)] = 1;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
