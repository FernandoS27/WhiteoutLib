// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/interpolate.h>

#include <whiteout/models/wem/geometry/triangulation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

/// The heaviest weight's index, the lower one on a tie.
u32 heaviest(std::span<const f32> weights) {
    u32 best = 0;
    for (u32 i = 1; i < weights.size(); ++i) {
        if (weights[i] > weights[best]) {
            best = i;
        }
    }
    return best;
}

f32 readF32(const u8* bytes, u32 component) {
    f32 value = 0.0f;
    std::memcpy(&value, bytes + sizeof(f32) * component, sizeof(f32));
    return value;
}

void writeF32(u8* bytes, u32 component, f32 value) {
    std::memcpy(bytes + sizeof(f32) * component, &value, sizeof(f32));
}

/// Renormalises the first three floats of @p bytes in place; a zero vector is
/// left as it is.
void renormalise3(u8* bytes) {
    const f32 x = readF32(bytes, 0);
    const f32 y = readF32(bytes, 1);
    const f32 z = readF32(bytes, 2);
    const f32 length = std::sqrt(x * x + y * y + z * z);
    if (length <= 1e-20f) {
        return;
    }
    writeF32(bytes, 0, x / length);
    writeF32(bytes, 1, y / length);
    writeF32(bytes, 2, z / length);
}

/// One value of @p type blended from @p count values `stride` bytes apart
/// (`values` is corner-major) into @p out, by `BlendCorners`' rules.
void blendValue(const std::string& name, AttrType type, const u8* values, u32 count,
                std::span<const f32> weights, u8* out) {
    const u32 stride = AttrTypeSize(type);
    const u32 top = heaviest(weights.first(count));
    switch (type) {
    case AttrType::F32:
    case AttrType::F32x2:
    case AttrType::F32x3:
    case AttrType::F32x4: {
        const u32 components = AttrTypeComponents(type);
        for (u32 c = 0; c < components; ++c) {
            f32 sum = 0.0f;
            for (u32 i = 0; i < count; ++i) {
                sum += weights[i] * readF32(values + stride * i, c);
            }
            writeF32(out, c, sum);
        }
        if (name == names::kNormal || name == names::kBinormal) {
            renormalise3(out);
        } else if (name == names::kTangent && components == 4) {
            renormalise3(out);
            writeF32(out, 3, readF32(values + stride * top, 3));
        }
        break;
    }
    case AttrType::U8x4:
        for (u32 c = 0; c < 4; ++c) {
            f32 sum = 0.0f;
            for (u32 i = 0; i < count; ++i) {
                sum += weights[i] * static_cast<f32>(values[stride * i + c]);
            }
            out[c] = static_cast<u8>(std::floor(std::clamp(sum, 0.0f, 255.0f) + 0.5f));
        }
        break;
    case AttrType::Quat: {
        // Aligned with the heaviest first: q and -q are one rotation, and
        // summing them cancels it.
        const u8* reference = values + stride * top;
        f32 sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (u32 i = 0; i < count; ++i) {
            f32 dot = 0.0f;
            for (u32 c = 0; c < 4; ++c) {
                dot += readF32(values + stride * i, c) * readF32(reference, c);
            }
            const f32 sign = dot < 0.0f ? -1.0f : 1.0f;
            for (u32 c = 0; c < 4; ++c) {
                sum[c] += sign * weights[i] * readF32(values + stride * i, c);
            }
        }
        const f32 length =
            std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2] + sum[3] * sum[3]);
        if (length <= 1e-20f) {
            std::memcpy(out, reference, stride);
            break;
        }
        for (u32 c = 0; c < 4; ++c) {
            writeF32(out, c, sum[c] / length);
        }
        break;
    }
    default:
        // Discrete values do not blend: the heaviest corner's.
        std::memcpy(out, values + stride * top, stride);
        break;
    }
}

} // namespace

SourcePolygon CaptureCorners(const Mesh& mesh, std::span<const HalfedgeId> corners) {
    SourcePolygon out;
    if (!mesh.hasConnectivity()) {
        return out;
    }
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
    for (const HalfedgeId h : corners) {
        const u32 vertex = static_cast<u32>(topology.from(h).index());
        out.vertices.push_back(vertex);
        out.positions.push_back(vertex < positions.size() ? positions[vertex]
                                                          : Vector3f{0.0f, 0.0f, 0.0f});
        std::vector<Influence> influences;
        if (!mesh.skin.empty() && vertex < mesh.skin.vertexCount()) {
            const std::span<const Influence> bound = mesh.skin.forVertex(vertex);
            influences.assign(bound.begin(), bound.end());
        }
        out.skin.push_back(std::move(influences));
    }
    for (const AttrLayer& layer : mesh.attributes.layers()) {
        if (layer.domain != Domain::Halfedge) {
            continue;
        }
        SourcePolygon::Layer copy;
        copy.name = layer.name;
        copy.type = layer.type;
        const std::size_t stride = AttrTypeSize(layer.type);
        copy.values.assign(stride * corners.size(), 0);
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const std::size_t from = stride * corners[i].index();
            if (from + stride <= layer.data.size()) {
                std::memcpy(copy.values.data() + stride * i, layer.data.data() + from, stride);
            }
        }
        out.layers.push_back(std::move(copy));
    }
    if (out.vertices.size() >= 3) {
        TriangulateFace(out.vertices, positions, {}, out.cut);
    }
    return out;
}

SourcePolygon CapturePolygon(const Mesh& mesh, FaceId face) {
    if (!mesh.hasConnectivity() || face.index() >= mesh.topology().faceCount() ||
        mesh.topology().isDeleted(face)) {
        return {};
    }
    std::vector<HalfedgeId> corners;
    for (const HalfedgeId h : mesh.topology().fh(face)) {
        corners.push_back(h);
    }
    SourcePolygon out = CaptureCorners(mesh, corners);
    out.cut.clear();
    TriangulateFace(out.vertices,
                    mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex),
                    mesh.triangulation.row(face.value()), out.cut);
    return out;
}

namespace {

struct Flat {
    f64 x = 0.0;
    f64 y = 0.0;
};

f64 cross2(const Flat& a, const Flat& b) {
    return a.x * b.y - a.y * b.x;
}

f64 dot2(const Flat& a, const Flat& b) {
    return a.x * b.x + a.y * b.y;
}

} // namespace

bool SampleWeights(std::span<const Vector3f> polygon, std::span<const u32> cut,
                   const Vector3f& point, std::span<f32> out) {
    std::fill(out.begin(), out.end(), 0.0f);
    const std::size_t n = std::min(polygon.size(), out.size());
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        out[0] = 1.0f;
        return true;
    }
    const auto diff = [](const Vector3f& a, const Vector3f& b) {
        return std::array<f64, 3>{static_cast<f64>(a.x) - b.x, static_cast<f64>(a.y) - b.y,
                                  static_cast<f64>(a.z) - b.z};
    };
    const auto length2 = [](const std::array<f64, 3>& v) {
        return v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    };
    f64 longest = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        longest = std::max(longest, length2(diff(polygon[(i + 1) % n], polygon[i])));
    }
    const f64 eps = 1e-6 * std::sqrt(longest);

    // --- on a corner --------------------------------------------------------
    for (std::size_t i = 0; i < n; ++i) {
        if (length2(diff(point, polygon[i])) <= eps * eps) {
            out[i] = 1.0f;
            return true;
        }
    }

    // --- an edge: the lerp along its line -------------------------------------
    if (n == 2) {
        const std::array<f64, 3> along = diff(polygon[1], polygon[0]);
        const std::array<f64, 3> to = diff(point, polygon[0]);
        const f64 l2 = length2(along);
        const f64 t = l2 > 0.0 ? (to[0] * along[0] + to[1] * along[1] + to[2] * along[2]) / l2 : 0.0;
        out[0] = static_cast<f32>(1.0 - t);
        out[1] = static_cast<f32>(t);
        return t >= 0.0 && t <= 1.0;
    }

    // --- the Newell plane -----------------------------------------------------
    std::array<f64, 3> normal{0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < n; ++i) {
        const Vector3f& a = polygon[i];
        const Vector3f& b = polygon[(i + 1) % n];
        normal[0] += (static_cast<f64>(a.y) - b.y) * (static_cast<f64>(a.z) + b.z);
        normal[1] += (static_cast<f64>(a.z) - b.z) * (static_cast<f64>(a.x) + b.x);
        normal[2] += (static_cast<f64>(a.x) - b.x) * (static_cast<f64>(a.y) + b.y);
    }
    const f64 area2 = std::sqrt(length2(normal));
    if (!(area2 > 2e-6 * longest)) {
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < n; ++i) {
            if (length2(diff(point, polygon[i])) < length2(diff(point, polygon[nearest]))) {
                nearest = i;
            }
        }
        out[nearest] = 1.0f;
        return false;
    }
    for (f64& c : normal) {
        c /= area2;
    }
    std::array<f64, 3> u{0.0, 0.0, 0.0};
    const std::size_t axis = std::abs(normal[0]) <= std::abs(normal[1]) &&
                                     std::abs(normal[0]) <= std::abs(normal[2])
                                 ? 0
                                 : (std::abs(normal[1]) <= std::abs(normal[2]) ? 1 : 2);
    u[axis] = 1.0;
    const f64 along = u[0] * normal[0] + u[1] * normal[1] + u[2] * normal[2];
    for (u32 c = 0; c < 3; ++c) {
        u[c] -= normal[c] * along;
    }
    const f64 ul = std::sqrt(length2(u));
    for (f64& c : u) {
        c /= ul;
    }
    const std::array<f64, 3> v{normal[1] * u[2] - normal[2] * u[1],
                               normal[2] * u[0] - normal[0] * u[2],
                               normal[0] * u[1] - normal[1] * u[0]};
    const auto flatten = [&](const Vector3f& p) {
        const std::array<f64, 3> d = diff(p, polygon[0]);
        return Flat{d[0] * u[0] + d[1] * u[1] + d[2] * u[2], d[0] * v[0] + d[1] * v[1] + d[2] * v[2]};
    };
    std::vector<Flat> corners(n);
    for (std::size_t i = 0; i < n; ++i) {
        corners[i] = flatten(polygon[i]);
    }
    const Flat x = flatten(point);

    // --- on an edge -----------------------------------------------------------
    for (std::size_t i = 0; i < n; ++i) {
        const Flat& a = corners[i];
        const Flat& b = corners[(i + 1) % n];
        const Flat ab{b.x - a.x, b.y - a.y};
        const Flat ax{x.x - a.x, x.y - a.y};
        const f64 l2 = dot2(ab, ab);
        if (l2 <= 0.0) {
            continue;
        }
        const f64 t = dot2(ax, ab) / l2;
        const f64 off = std::abs(cross2(ab, ax)) / std::sqrt(l2);
        if (t >= 0.0 && t <= 1.0 && off <= eps) {
            out[i] = static_cast<f32>(1.0 - t);
            out[(i + 1) % n] = static_cast<f32>(t);
            return true;
        }
    }

    // --- inside: barycentric in the drawn triangle that holds it --------------
    for (std::size_t k = 0; k + 2 < cut.size(); k += 3) {
        const u32 ia = cut[k];
        const u32 ib = cut[k + 1];
        const u32 ic = cut[k + 2];
        if (ia >= n || ib >= n || ic >= n) {
            continue;
        }
        const Flat& a = corners[ia];
        const Flat& b = corners[ib];
        const Flat& c = corners[ic];
        const f64 whole = cross2(Flat{b.x - a.x, b.y - a.y}, Flat{c.x - a.x, c.y - a.y});
        if (whole == 0.0) {
            continue;
        }
        const f64 wa = cross2(Flat{b.x - x.x, b.y - x.y}, Flat{c.x - x.x, c.y - x.y}) / whole;
        const f64 wb = cross2(Flat{c.x - x.x, c.y - x.y}, Flat{a.x - x.x, a.y - x.y}) / whole;
        const f64 wc = 1.0 - wa - wb;
        const f64 slack = -1e-9;
        if (wa >= slack && wb >= slack && wc >= slack) {
            out[ia] = static_cast<f32>(wa);
            out[ib] = static_cast<f32>(wb);
            out[ic] = static_cast<f32>(wc);
            return true;
        }
    }

    // --- outside: mean-value weights ------------------------------------------
    std::vector<Flat> r(n);
    std::vector<f64> length(n);
    for (std::size_t i = 0; i < n; ++i) {
        r[i] = Flat{corners[i].x - x.x, corners[i].y - x.y};
        length[i] = std::sqrt(dot2(r[i], r[i]));
    }
    std::vector<f64> halfTan(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        const f64 denominator = length[i] * length[j] + dot2(r[i], r[j]);
        halfTan[i] = denominator != 0.0 ? cross2(r[i], r[j]) / denominator : 0.0;
    }
    std::vector<f64> w(n);
    f64 sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        w[i] = (halfTan[(i + n - 1) % n] + halfTan[i]) / length[i];
        sum += w[i];
    }
    if (sum == 0.0 || !std::isfinite(sum)) {
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < n; ++i) {
            if (length[i] < length[nearest]) {
                nearest = i;
            }
        }
        out[nearest] = 1.0f;
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<f32>(w[i] / sum);
    }
    return false;
}

void BlendCorners(Mesh& mesh, const SourcePolygon& source, std::span<const f32> weights,
                  std::span<const HalfedgeId> targets) {
    const u32 count = std::min<u32>(source.cornerCount(), static_cast<u32>(weights.size()));
    if (count == 0 || targets.empty()) {
        return;
    }
    std::vector<u8> blended;
    for (const SourcePolygon::Layer& from : source.layers) {
        AttrLayer* layer = mesh.attributes.layer(from.name, Domain::Halfedge);
        if (layer == nullptr || layer->type != from.type) {
            continue;
        }
        const u32 stride = AttrTypeSize(from.type);
        if (stride == 0 || from.values.size() < std::size_t{stride} * count) {
            continue;
        }
        blended.assign(stride, 0);
        blendValue(from.name, from.type, from.values.data(), count, weights, blended.data());
        for (const HalfedgeId target : targets) {
            const std::size_t at = std::size_t{stride} * target.index();
            if (at + stride <= layer->data.size()) {
                std::memcpy(layer->data.data() + at, blended.data(), stride);
            }
        }
    }
}

std::vector<Influence> BlendInfluences(std::span<const std::vector<Influence>> sources,
                                       std::span<const f32> weights) {
    const std::size_t count = std::min(sources.size(), weights.size());
    if (count == 0) {
        return {};
    }
    std::vector<f32> kept(count, 0.0f);
    f32 total = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        kept[i] = weights[i] > 0.0f ? weights[i] : 0.0f; // NaN compares false too
        total += kept[i];
    }
    if (!(total > 0.0f)) {
        // Every weight at or under zero: the corner the point is least far out
        // from answers alone.
        kept.assign(count, 0.0f);
        kept[heaviest(weights.first(count))] = 1.0f;
        total = 1.0f;
    }

    std::vector<Influence> sum;
    for (std::size_t i = 0; i < count; ++i) {
        if (kept[i] == 0.0f) {
            continue;
        }
        const f32 share = kept[i] / total;
        for (const Influence& influence : sources[i]) {
            const auto found = std::find_if(sum.begin(), sum.end(), [&](const Influence& s) {
                return s.bone == influence.bone;
            });
            if (found != sum.end()) {
                found->weight += share * influence.weight;
            } else {
                sum.push_back(Influence{influence.bone, share * influence.weight});
            }
        }
    }
    std::sort(sum.begin(), sum.end(), [](const Influence& a, const Influence& b) {
        return a.weight != b.weight ? a.weight > b.weight : a.bone < b.bone;
    });
    std::erase_if(sum, [](const Influence& i) { return !(i.weight >= kInfluencePrune); });
    if (sum.size() > kMaxBlendedInfluences) {
        sum.resize(kMaxBlendedInfluences);
    }
    f32 remaining = 0.0f;
    for (const Influence& influence : sum) {
        remaining += influence.weight;
    }
    if (remaining > 0.0f) {
        for (Influence& influence : sum) {
            influence.weight /= remaining;
        }
    }
    return sum;
}

std::vector<Influence> BlendInfluences(const SourcePolygon& source, std::span<const f32> weights) {
    return BlendInfluences(std::span<const std::vector<Influence>>(source.skin), weights);
}

void BlendVertex(Mesh& mesh, std::span<const u32> vertices, std::span<const f32> weights,
                 u32 target) {
    const u32 count = static_cast<u32>(std::min(vertices.size(), weights.size()));
    if (count == 0) {
        return;
    }
    std::vector<u8> gathered;
    std::vector<u8> blended;
    for (const AttrLayer& layer : mesh.attributes.layers()) {
        if (layer.domain != Domain::Vertex || layer.name == names::kPosition ||
            layer.name == names::kMergeGroup) {
            continue;
        }
        const u32 stride = AttrTypeSize(layer.type);
        if (stride == 0 || std::size_t{stride} * (target + 1) > layer.data.size()) {
            continue;
        }
        gathered.assign(std::size_t{stride} * count, 0);
        for (u32 i = 0; i < count; ++i) {
            const std::size_t from = std::size_t{stride} * vertices[i];
            if (from + stride <= layer.data.size()) {
                std::memcpy(gathered.data() + std::size_t{stride} * i, layer.data.data() + from,
                            stride);
            }
        }
        blended.assign(stride, 0);
        blendValue(layer.name, layer.type, gathered.data(), count, weights, blended.data());
        AttrLayer& written = *mesh.attributes.layer(layer.name, Domain::Vertex);
        std::memcpy(written.data.data() + std::size_t{stride} * target, blended.data(), stride);
    }

    if (!mesh.skin.empty() && target < mesh.skin.vertexCount()) {
        std::vector<std::vector<Influence>> sources(count);
        for (u32 i = 0; i < count; ++i) {
            if (vertices[i] < mesh.skin.vertexCount()) {
                const std::span<const Influence> bound = std::as_const(mesh.skin).forVertex(vertices[i]);
                sources[i].assign(bound.begin(), bound.end());
            }
        }
        const std::vector<Influence> skin =
            BlendInfluences(std::span<const std::vector<Influence>>(sources), weights);
        mesh.skin.assignVertex(target, skin);
    }
}

u32 FreshMergeGroup(const Mesh& mesh) {
    const std::span<const u32> groups =
        mesh.attributes.get<const u32>(names::kMergeGroup, Domain::Vertex);
    u32 next = mesh.vertexCount();
    for (const u32 group : groups) {
        next = std::max(next, group + 1);
    }
    return next;
}

std::span<u32> MergeGroupsOf(Mesh& mesh) {
    const bool had = mesh.attributes.has(names::kMergeGroup, Domain::Vertex);
    const std::span<u32> groups =
        mesh.attributes.getOrCreate<u32>(names::kMergeGroup, Domain::Vertex, AttrType::U32);
    if (!had) {
        for (u32 v = 0; v < groups.size(); ++v) {
            groups[v] = v;
        }
    }
    return groups;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
