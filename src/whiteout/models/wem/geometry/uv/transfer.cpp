// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/uv/transfer.h>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/triangulation.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {

namespace {

using textures::PixelFormat;
using textures::Texture;
using textures::TextureKind;

// ---- colour -------------------------------------------------------------------

/// IEC 61966-2-1, in float: the library's own tables are 8-bit, and a bake that
/// rounds on the way in and again on the way out is not byte-exact.
f32 toLinear(f32 s) {
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

f32 toSrgb(f32 l) {
    return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

u8 toByte(f32 v) {
    return static_cast<u8>(std::clamp(std::lround(v * 255.0f), 0l, 255l));
}

f32 fromByte(u8 v) {
    return static_cast<f32>(v) / 255.0f;
}

Vector3f normalized(const Vector3f& v) {
    const f32 length = v.length();
    return length > 1e-20f ? v * (1.0f / length) : Vector3f{0.0f, 0.0f, 1.0f};
}

// ---- the source, decoded ------------------------------------------------------

/// One level of the source in float: linear colour, or for a normal map the
/// unit direction in xyz and the channel the packing leaves spare in w.
struct Level {
    u32 width = 0;
    u32 height = 0;
    std::vector<Vector4f> texels;
};

/// The source's pyramid, built here on the decoded copy: the texture's own
/// mips of a normal map are packed and unpacked by a filter this cannot see
/// through, so what they hold is not the data a sample here expects.
struct Pyramid {
    std::vector<Level> levels;
    bool normal = false;
    bool wrapU = true;
    bool wrapV = true;
};

std::optional<Pyramid> decode(const TransferSource& source) {
    const Texture& image = *source.image;
    if (image.width() == 0 || image.height() == 0) {
        return std::nullopt;
    }
    const Texture rgba = image.format() == PixelFormat::RGBA8 ? image
                                                               : image.copyAsFormat(PixelFormat::RGBA8);
    const std::span<const u8> bytes = rgba.mipData(0);
    const u32 width = rgba.width();
    const u32 height = rgba.height();
    if (bytes.size() < static_cast<std::size_t>(width) * height * 4) {
        return std::nullopt;
    }

    Pyramid out;
    out.normal = image.kind() == TextureKind::Normal;
    out.wrapU = source.wrapU;
    out.wrapV = source.wrapV;
    Level base;
    base.width = width;
    base.height = height;
    base.texels.resize(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < base.texels.size(); ++i) {
        const u8* p = bytes.data() + i * 4;
        if (out.normal) {
            // The packing undone: a direction in xyz, whatever the file spelled.
            f32 x = fromByte(p[0]) * 2.0f - 1.0f;
            f32 y = fromByte(p[1]) * 2.0f - 1.0f;
            f32 z = fromByte(p[2]) * 2.0f - 1.0f;
            f32 spare = fromByte(p[3]);
            if (source.packing == NormalPacking::XInAlpha) {
                x = fromByte(p[3]) * 2.0f - 1.0f;
                spare = 1.0f;
            }
            if (source.packing != NormalPacking::None) {
                z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
            }
            const Vector3f n = normalized(Vector3f{x, y, z});
            base.texels[i] = Vector4f{n.x, n.y, n.z, spare};
        } else {
            const f32 r = fromByte(p[0]);
            const f32 g = fromByte(p[1]);
            const f32 b = fromByte(p[2]);
            base.texels[i] = source.srgb ? Vector4f{toLinear(r), toLinear(g), toLinear(b), fromByte(p[3])}
                                         : Vector4f{r, g, b, fromByte(p[3])};
        }
    }
    out.levels.push_back(std::move(base));

    // Box-halved down to one texel: the level a sample reads is chosen by how
    // much smaller the target is, so it only has to be the right average.
    while (out.levels.back().width > 1 || out.levels.back().height > 1) {
        const Level& above = out.levels.back();
        Level next;
        next.width = std::max(1u, above.width / 2);
        next.height = std::max(1u, above.height / 2);
        next.texels.resize(static_cast<std::size_t>(next.width) * next.height);
        for (u32 y = 0; y < next.height; ++y) {
            for (u32 x = 0; x < next.width; ++x) {
                Vector4f sum{0.0f, 0.0f, 0.0f, 0.0f};
                for (u32 dy = 0; dy < 2; ++dy) {
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u32 sx = std::min(above.width - 1, x * 2 + dx);
                        const u32 sy = std::min(above.height - 1, y * 2 + dy);
                        sum = sum + above.texels[static_cast<std::size_t>(sy) * above.width + sx];
                    }
                }
                Vector4f mean = sum * 0.25f;
                if (out.normal) {
                    const Vector3f n = normalized(Vector3f{mean.x, mean.y, mean.z});
                    mean = Vector4f{n.x, n.y, n.z, mean.w};
                }
                next.texels[static_cast<std::size_t>(y) * next.width + x] = mean;
            }
        }
        out.levels.push_back(std::move(next));
    }
    return out;
}

i32 tap(i32 i, u32 size, bool wrap) {
    const i32 n = static_cast<i32>(size);
    return wrap ? ((i % n) + n) % n : std::clamp(i, 0, n - 1);
}

/// Bilinear, in float, with the texel centres at half-integers: a UV that
/// lands on a centre reads that texel and nothing else.
Vector4f sample(const Pyramid& pyramid, u32 level, const Vector2f& uv) {
    const Level& l = pyramid.levels[level];
    const f32 x = uv.x * static_cast<f32>(l.width) - 0.5f;
    const f32 y = uv.y * static_cast<f32>(l.height) - 0.5f;
    const f32 fx0 = std::floor(x);
    const f32 fy0 = std::floor(y);
    const f32 ax = x - fx0;
    const f32 ay = y - fy0;
    const i32 x0 = static_cast<i32>(fx0);
    const i32 y0 = static_cast<i32>(fy0);
    const auto at = [&](i32 tx, i32 ty) {
        const i32 cx = tap(tx, l.width, pyramid.wrapU);
        const i32 cy = tap(ty, l.height, pyramid.wrapV);
        return l.texels[static_cast<std::size_t>(cy) * l.width + static_cast<std::size_t>(cx)];
    };
    const Vector4f top = at(x0, y0) * (1.0f - ax) + at(x0 + 1, y0) * ax;
    const Vector4f bottom = at(x0, y0 + 1) * (1.0f - ax) + at(x0 + 1, y0 + 1) * ax;
    return top * (1.0f - ay) + bottom * ay;
}

// ---- the tangent frames --------------------------------------------------------

/// A triangle's tangent frame in one set, orthonormal, with its handedness in
/// the bitangent: what a normal map on that set was painted against.
struct Frame {
    Vector3f t{1.0f, 0.0f, 0.0f};
    Vector3f b{0.0f, 1.0f, 0.0f};
    Vector3f n{0.0f, 0.0f, 1.0f};
    bool valid = false;
};

Frame frameOf(const Vector3f (&p)[3], const Vector2f (&uv)[3]) {
    Frame f;
    const Vector3f e1 = p[1] - p[0];
    const Vector3f e2 = p[2] - p[0];
    const Vector2f d1 = uv[1] - uv[0];
    const Vector2f d2 = uv[2] - uv[0];
    const f32 r = d1.x * d2.y - d2.x * d1.y;
    const Vector3f n = cross(e1, e2);
    if (std::abs(r) < 1e-20f || n.length() < 1e-20f) {
        return f;
    }
    f.n = normalized(n);
    const Vector3f t = (e1 * d2.y - e2 * d1.y) * (1.0f / r);
    const Vector3f b = (e2 * d1.x - e1 * d2.x) * (1.0f / r);
    f.t = normalized(t - f.n * f.n.dot(t));
    const f32 handed = cross(f.n, f.t).dot(b) < 0.0f ? -1.0f : 1.0f;
    f.b = cross(f.n, f.t) * handed;
    f.valid = true;
    return f;
}

// ---- the raster ----------------------------------------------------------------

f32 edgeFunction(const Vector2f& a, const Vector2f& b, const Vector2f& p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

/// Which of two triangles sharing an edge owns a texel centre exactly on it.
/// Any rule that gives an edge and its reverse opposite answers does; this is
/// the usual one.
bool ownsEdge(const Vector2f& a, const Vector2f& b) {
    const Vector2f d = b - a;
    return d.y > 0.0f || (d.y == 0.0f && d.x < 0.0f);
}

Vector2f closestOnSegment(const Vector2f& a, const Vector2f& b, const Vector2f& p) {
    const Vector2f d = b - a;
    const f32 len2 = d.x * d.x + d.y * d.y;
    const f32 t = len2 > 0.0f ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / len2, 0.0f, 1.0f)
                              : 0.0f;
    return a + d * t;
}

f32 distance2(const Vector2f& a, const Vector2f& b) {
    const Vector2f d = a - b;
    return d.x * d.x + d.y * d.y;
}

/// What the raster holds per texel.
enum class Mark : u8 { Empty, Ring, Core, Dilated };

struct Target {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
    std::vector<Mark> mark;
    std::vector<u32> owner;
};

/// One triangle of one mesh, everything a texel inside it needs.
struct Triangle {
    Vector2f to[3];   ///< In target texels.
    Vector2f from[3]; ///< In source UV.
    Frame source;
    Frame target;
    u32 owner = kInvalidId;
};

/// The encoded bytes of the source at barycentric @p w of @p tri.
void shade(const Pyramid& pyramid, u32 level, const TransferSource& source, const Triangle& tri,
           const f32 (&w)[3], u8 (&out)[4]) {
    const Vector2f uv = tri.from[0] * w[0] + tri.from[1] * w[1] + tri.from[2] * w[2];
    const Vector4f s = sample(pyramid, level, uv);
    if (pyramid.normal) {
        Vector3f n = normalized(Vector3f{s.x, s.y, s.z});
        if (tri.source.valid && tri.target.valid) {
            // Into the surface through the frame it was painted in, and back
            // out through the one it will be read in.
            const Vector3f world = tri.source.t * n.x + tri.source.b * n.y + tri.source.n * n.z;
            n = normalized(Vector3f{world.dot(tri.target.t), world.dot(tri.target.b),
                                    world.dot(tri.target.n)});
        }
        const u8 x = toByte(n.x * 0.5f + 0.5f);
        const u8 y = toByte(n.y * 0.5f + 0.5f);
        const u8 z = toByte(n.z * 0.5f + 0.5f);
        // The packing restated: the image goes back spelled as it came.
        if (source.packing == NormalPacking::XInAlpha) {
            out[0] = 255;
            out[1] = y;
            out[2] = 0;
            out[3] = x;
        } else {
            out[0] = x;
            out[1] = y;
            out[2] = z;
            out[3] = toByte(s.w);
        }
        return;
    }
    if (source.srgb) {
        out[0] = toByte(toSrgb(s.x));
        out[1] = toByte(toSrgb(s.y));
        out[2] = toByte(toSrgb(s.z));
    } else {
        out[0] = toByte(s.x);
        out[1] = toByte(s.y);
        out[2] = toByte(s.z);
    }
    out[3] = toByte(s.w);
}

/// Rasterises @p tri: every texel centre inside it by the edge rule, then the
/// texels its outline only grazes, from the nearest point of it. The graze is
/// what keeps an island's edge from reading the empty tile when the map is
/// filtered.
void rasterise(Target& target, const Pyramid& pyramid, u32 level, const TransferSource& source,
               const Triangle& input, TransferReport& report) {
    Triangle tri = input;
    f32 area = edgeFunction(tri.to[0], tri.to[1], tri.to[2]);
    if (area < 0.0f) {
        std::swap(tri.to[1], tri.to[2]);
        std::swap(tri.from[1], tri.from[2]);
        area = -area;
    }
    const f32 minX = std::min({tri.to[0].x, tri.to[1].x, tri.to[2].x});
    const f32 maxX = std::max({tri.to[0].x, tri.to[1].x, tri.to[2].x});
    const f32 minY = std::min({tri.to[0].y, tri.to[1].y, tri.to[2].y});
    const f32 maxY = std::max({tri.to[0].y, tri.to[1].y, tri.to[2].y});
    const i32 x0 = std::max(0, static_cast<i32>(std::floor(minX)) - 1);
    const i32 y0 = std::max(0, static_cast<i32>(std::floor(minY)) - 1);
    const i32 x1 = std::min(static_cast<i32>(target.width) - 1, static_cast<i32>(std::ceil(maxX)) + 1);
    const i32 y1 = std::min(static_cast<i32>(target.height) - 1, static_cast<i32>(std::ceil(maxY)) + 1);
    const bool owns[3] = {ownsEdge(tri.to[1], tri.to[2]), ownsEdge(tri.to[2], tri.to[0]),
                          ownsEdge(tri.to[0], tri.to[1])};
    // Half a texel's diagonal: a texel whose square the outline touches.
    constexpr f32 kGraze2 = 0.5f;

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const Vector2f p{static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f};
            const f32 e[3] = {edgeFunction(tri.to[1], tri.to[2], p),
                              edgeFunction(tri.to[2], tri.to[0], p),
                              edgeFunction(tri.to[0], tri.to[1], p)};
            const bool inside = (e[0] > 0.0f || (e[0] == 0.0f && owns[0])) &&
                                (e[1] > 0.0f || (e[1] == 0.0f && owns[1])) &&
                                (e[2] > 0.0f || (e[2] == 0.0f && owns[2]));
            const std::size_t at = static_cast<std::size_t>(y) * target.width + static_cast<std::size_t>(x);
            f32 w[3];
            if (inside) {
                w[0] = e[0] / area;
                w[1] = e[1] / area;
                w[2] = e[2] / area;
            } else {
                if (target.mark[at] != Mark::Empty) {
                    continue;
                }
                Vector2f best = closestOnSegment(tri.to[0], tri.to[1], p);
                for (const Vector2f& c : {closestOnSegment(tri.to[1], tri.to[2], p),
                                          closestOnSegment(tri.to[2], tri.to[0], p)}) {
                    if (distance2(c, p) < distance2(best, p)) {
                        best = c;
                    }
                }
                if (distance2(best, p) > kGraze2) {
                    continue;
                }
                w[0] = std::max(0.0f, edgeFunction(tri.to[1], tri.to[2], best) / area);
                w[1] = std::max(0.0f, edgeFunction(tri.to[2], tri.to[0], best) / area);
                w[2] = std::max(0.0f, 1.0f - w[0] - w[1]);
            }

            u8 value[4];
            shade(pyramid, level, source, tri, w, value);
            u8* slot = target.rgba.data() + at * 4;
            if (!inside) {
                std::copy(value, value + 4, slot);
                target.mark[at] = Mark::Ring;
                target.owner[at] = tri.owner;
                continue;
            }
            if (target.mark[at] == Mark::Core) {
                // One island's own triangles meet here: the edge rule already
                // gave the texel to one of them.
                if (target.owner[at] == tri.owner) {
                    continue;
                }
                // A second island over a texel another already wrote: a stacked
                // target. The last island in island order wins (D§9.3 step 5),
                // and a disagreement is counted.
                if (!std::equal(value, value + 4, slot)) {
                    ++report.twiceDiffering;
                }
                std::copy(value, value + 4, slot);
                target.owner[at] = tri.owner;
                continue;
            }
            std::copy(value, value + 4, slot);
            target.mark[at] = Mark::Core;
            target.owner[at] = tri.owner;
            ++report.written;
        }
    }
}

/// @p padding rings outward from every written texel, each empty texel taking
/// its nearest written neighbour's value: the sides first, then the corners.
void dilate(Target& target, u32 padding, TransferReport& report) {
    constexpr i32 kNeighbours[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                       {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    const i32 width = static_cast<i32>(target.width);
    const i32 height = static_cast<i32>(target.height);
    std::vector<std::size_t> ring;
    for (u32 pass = 0; pass < padding; ++pass) {
        ring.clear();
        std::vector<std::size_t> sourceOf;
        for (i32 y = 0; y < height; ++y) {
            for (i32 x = 0; x < width; ++x) {
                const std::size_t at = static_cast<std::size_t>(y) * target.width + static_cast<std::size_t>(x);
                if (target.mark[at] != Mark::Empty) {
                    continue;
                }
                for (const auto& d : kNeighbours) {
                    const i32 nx = x + d[0];
                    const i32 ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        continue;
                    }
                    const std::size_t from = static_cast<std::size_t>(ny) * target.width + static_cast<std::size_t>(nx);
                    if (target.mark[from] != Mark::Empty) {
                        ring.push_back(at);
                        sourceOf.push_back(from);
                        break;
                    }
                }
            }
        }
        if (ring.empty()) {
            return;
        }
        // Written after the scan, so a ring grows by one texel and not along a
        // whole row in one pass.
        for (std::size_t i = 0; i < ring.size(); ++i) {
            std::copy(target.rgba.begin() + static_cast<std::ptrdiff_t>(sourceOf[i] * 4),
                      target.rgba.begin() + static_cast<std::ptrdiff_t>(sourceOf[i] * 4 + 4),
                      target.rgba.begin() + static_cast<std::ptrdiff_t>(ring[i] * 4));
            target.mark[ring[i]] = Mark::Dilated;
        }
        report.dilated += static_cast<u32>(ring.size());
    }
}

} // namespace

std::optional<Texture> TransferTexture(std::span<const TransferMesh> meshes,
                                       const TransferSource& source,
                                       const TransferOptions& options, TransferReport& report) {
    report = TransferReport{};
    if (source.image == nullptr) {
        return std::nullopt;
    }
    const std::optional<Pyramid> pyramid = decode(source);
    if (!pyramid.has_value()) {
        return std::nullopt;
    }

    Target target;
    target.width = options.width != 0 ? options.width : source.image->width();
    target.height = options.height != 0 ? options.height : source.image->height();
    const std::size_t texels = static_cast<std::size_t>(target.width) * target.height;
    target.rgba.assign(texels * 4, 0);
    target.mark.assign(texels, Mark::Empty);
    target.owner.assign(texels, kInvalidId);

    // The level nearest the size ratio, so a small target reads an average
    // rather than aliasing the texels a large source has between its samples.
    const f32 ratio = std::max(static_cast<f32>(source.image->width()) / static_cast<f32>(target.width),
                               static_cast<f32>(source.image->height()) / static_cast<f32>(target.height));
    const u32 level = ratio > 1.0f ? std::min(static_cast<u32>(pyramid->levels.size() - 1),
                                              static_cast<u32>(std::lround(std::log2(ratio))))
                                   : 0u;

    u32 ownerBase = 0;
    std::vector<u32> loop;
    std::vector<HalfedgeId> corners;
    std::vector<u32> cut;
    for (const TransferMesh& entry : meshes) {
        if (entry.mesh == nullptr || !entry.mesh->hasConnectivity()) {
            continue;
        }
        const Mesh& mesh = *entry.mesh;
        const Topology& topology = mesh.topology();
        const std::span<const Vector3f> positions =
            mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
        const std::span<const Vector2f> fromUvs =
            mesh.attributes.get<const Vector2f>(names::uv(entry.from), Domain::Halfedge);
        const std::span<const Vector2f> toUvs =
            mesh.attributes.get<const Vector2f>(names::uv(entry.to), Domain::Halfedge);
        if (fromUvs.empty() || toUvs.empty() || positions.empty()) {
            continue;
        }
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            const FaceId face{f};
            if (topology.isDeleted(face)) {
                continue;
            }
            loop.clear();
            corners.clear();
            const HalfedgeId first = topology.halfedge(face);
            HalfedgeId walk = first;
            do {
                corners.push_back(walk);
                loop.push_back(topology.from(walk).value());
                walk = topology.next(walk);
            } while (walk != first && corners.size() < topology.halfedgeCount());
            if (corners.size() < 3) {
                continue;
            }
            cut.clear();
            TriangulateFace(loop, positions, mesh.triangulation.row(f), cut);

            const u32 island = entry.toIslands != nullptr ? entry.toIslands->islandOf(f) : 0u;
            bool drew = false;
            for (std::size_t i = 0; i + 2 < cut.size(); i += 3) {
                Triangle tri;
                tri.owner = ownerBase + (island == kInvalidId ? 0u : island);
                Vector3f p[3];
                Vector2f from[3];
                Vector2f to[3];
                bool inRange = true;
                for (u32 k = 0; k < 3; ++k) {
                    const HalfedgeId h = corners[cut[i + k]];
                    const u32 v = topology.from(h).index();
                    if (h.index() >= fromUvs.size() || h.index() >= toUvs.size() ||
                        v >= positions.size()) {
                        inRange = false;
                        break;
                    }
                    p[k] = positions[v];
                    from[k] = fromUvs[h.index()];
                    to[k] = toUvs[h.index()];
                    tri.from[k] = from[k];
                    tri.to[k] = Vector2f{to[k].x * static_cast<f32>(target.width),
                                         to[k].y * static_cast<f32>(target.height)};
                }
                if (!inRange) {
                    continue;
                }
                if (std::abs(edgeFunction(tri.to[0], tri.to[1], tri.to[2])) < 1e-12f) {
                    continue;
                }
                if (pyramid->normal) {
                    tri.source = frameOf(p, from);
                    tri.target = frameOf(p, to);
                }
                rasterise(target, *pyramid, level, source, tri, report);
                drew = true;
            }
            if (!drew) {
                ++report.unmappedFaces;
            }
        }
        ownerBase += entry.toIslands != nullptr ? std::max(1u, entry.toIslands->count) : 1u;
    }

    dilate(target, options.padding, report);

    Texture out = Texture::create2D(PixelFormat::RGBA8, target.width, target.height, 1);
    const std::span<u8> bytes = out.mipData(0);
    std::copy(target.rgba.begin(), target.rgba.begin() + static_cast<std::ptrdiff_t>(bytes.size()),
              bytes.begin());
    out.setKind(source.image->kind());
    out.setSrgb(source.srgb && !pyramid->normal);
    return out;
}

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
