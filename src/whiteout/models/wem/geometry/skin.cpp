// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/skin.h>

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

std::span<const Influence> SkinBinding::forVertex(u32 vertex) const {
    if (vertex + 1 >= offsets.size()) {
        return {};
    }
    const u32 begin = offsets[vertex];
    const u32 end = offsets[vertex + 1];
    return std::span<const Influence>(influences.data() + begin, end - begin);
}

std::span<Influence> SkinBinding::forVertex(u32 vertex) {
    if (vertex + 1 >= offsets.size()) {
        return {};
    }
    const u32 begin = offsets[vertex];
    const u32 end = offsets[vertex + 1];
    return std::span<Influence>(influences.data() + begin, end - begin);
}

u32 SkinBinding::maxInfluences() const {
    u32 widest = 0;
    for (std::size_t v = 0; v + 1 < offsets.size(); ++v) {
        widest = std::max(widest, offsets[v + 1] - offsets[v]);
    }
    return widest;
}

void SkinBinding::reset(u32 vertexCountIn) {
    offsets.assign(static_cast<std::size_t>(vertexCountIn) + 1, 0);
    influences.clear();
}

void SkinBinding::appendVertex(std::span<const Influence> values) {
    if (offsets.empty()) {
        offsets.push_back(0);
    }
    influences.insert(influences.end(), values.begin(), values.end());
    offsets.push_back(static_cast<u32>(influences.size()));
}

void SkinBinding::appendCopyOf(u32 source) {
    // Copy into a temporary first: `forVertex` returns a view into `influences`,
    // and the insert below can reallocate it.
    const auto view = forVertex(source);
    const std::vector<Influence> copy(view.begin(), view.end());
    appendVertex(std::span<const Influence>(copy.data(), copy.size()));
}

void SkinBinding::remapVertices(std::span<const u32> remap, u32 newCount) {
    if (offsets.empty()) {
        return;
    }
    std::vector<u32> newOffsets(static_cast<std::size_t>(newCount) + 1, 0);
    std::vector<Influence> rebuilt;
    rebuilt.reserve(influences.size());

    // Two passes: sizes first, so the CSR is filled in destination order rather
    // than in source order with a sort afterwards.
    for (std::size_t v = 0; v < remap.size() && v + 1 < offsets.size(); ++v) {
        const u32 fresh = remap[v];
        if (fresh == kInvalidId || fresh >= newCount) {
            continue;
        }
        newOffsets[fresh + 1] = offsets[v + 1] - offsets[v];
    }
    for (u32 v = 0; v < newCount; ++v) {
        newOffsets[v + 1] += newOffsets[v];
    }
    rebuilt.resize(newOffsets[newCount]);
    for (std::size_t v = 0; v < remap.size() && v + 1 < offsets.size(); ++v) {
        const u32 fresh = remap[v];
        if (fresh == kInvalidId || fresh >= newCount) {
            continue;
        }
        std::copy(influences.begin() + offsets[v], influences.begin() + offsets[v + 1],
                  rebuilt.begin() + newOffsets[fresh]);
    }

    offsets = std::move(newOffsets);
    influences = std::move(rebuilt);
}

void SkinBinding::normalize() {
    for (std::size_t v = 0; v + 1 < offsets.size(); ++v) {
        const u32 begin = offsets[v];
        const u32 end = offsets[v + 1];
        f32 total = 0;
        for (u32 i = begin; i < end; ++i) {
            total += influences[i].weight;
        }
        if (total <= 0.0f) {
            continue;
        }
        const f32 scale = 1.0f / total;
        for (u32 i = begin; i < end; ++i) {
            influences[i].weight *= scale;
        }
    }
}

bool SkinBinding::isNormalized(f32 tolerance) const {
    for (std::size_t v = 0; v + 1 < offsets.size(); ++v) {
        const u32 begin = offsets[v];
        const u32 end = offsets[v + 1];
        if (begin == end) {
            continue;
        }
        f32 total = 0;
        for (u32 i = begin; i < end; ++i) {
            total += influences[i].weight;
        }
        if (std::abs(total - 1.0f) > tolerance) {
            return false;
        }
    }
    return true;
}

void SkinBinding::sortByWeight() {
    for (std::size_t v = 0; v + 1 < offsets.size(); ++v) {
        const auto begin = influences.begin() + offsets[v];
        const auto end = influences.begin() + offsets[v + 1];
        // Stable, and the bone index breaks ties, so equal weights keep a
        // deterministic order rather than one that depends on the sort.
        std::stable_sort(begin, end, [](const Influence& a, const Influence& b) {
            if (a.weight != b.weight) {
                return a.weight > b.weight;
            }
            return a.bone < b.bone;
        });
    }
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
