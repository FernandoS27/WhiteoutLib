// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/fracture/cells.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "parallel.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace fracture {

namespace {

f64 Dot(const Vector3d& a, const Vector3d& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3d Scale(const Vector3d& a, const Vector3d& s) {
    return Vector3d(a.x * s.x, a.y * s.y, a.z * s.z);
}

Vector3d Times(const Vector3d& a, f64 s) {
    return Vector3d(a.x * s, a.y * s, a.z * s);
}

f64 Diagonal(const Vector3d& low, const Vector3d& high) {
    const Vector3d d = high - low;
    return std::sqrt(Dot(d, d));
}

/// The sites two vertices share, plus @p other: the sites of a vertex made on
/// the edge between them.
std::array<u32, 4> EdgeSites(const std::array<u32, 4>& a, const std::array<u32, 4>& b,
                             u32 other) {
    std::array<u32, 8> all{};
    u32 count = 0;
    for (u32 s : a) {
        if (s != kNoSite && std::find(b.begin(), b.end(), s) != b.end()) {
            all[count++] = s;
        }
    }
    if (std::find(all.begin(), all.begin() + count, other) == all.begin() + count) {
        all[count++] = other;
    }
    std::sort(all.begin(), all.begin() + count);
    std::array<u32, 4> out{kNoSite, kNoSite, kNoSite, kNoSite};
    for (u32 i = 0; i < count && i < 4; ++i) {
        out[i] = all[i];
    }
    return out;
}

} // namespace

u64 PlaneKey(u32 a, u32 b) {
    const u32 low = std::min(a, b);
    const u32 high = std::max(a, b);
    return (static_cast<u64>(low) << 32) | high;
}

u32 PlaneLow(u64 key) {
    return static_cast<u32>(key >> 32);
}

u32 PlaneHigh(u64 key) {
    return static_cast<u32>(key & 0xFFFFFFFFu);
}

ConvexCell BoxCell(const Vector3d& low, const Vector3d& high, u32 site) {
    ConvexCell cell;
    cell.site = site;
    for (u32 v = 0; v < 8; ++v) {
        const bool x = (v & 1) != 0;
        const bool y = (v & 2) != 0;
        const bool z = (v & 4) != 0;
        cell.vertices.push_back(Vector3d(x ? high.x : low.x, y ? high.y : low.y, z ? high.z : low.z));
        std::array<u32, 4> sites{site, kBoxSite + (x ? 1u : 0u), kBoxSite + 2 + (y ? 1u : 0u),
                                 kBoxSite + 4 + (z ? 1u : 0u)};
        std::sort(sites.begin(), sites.end());
        cell.sites.push_back(sites);
    }
    const u32 loops[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4},
                             {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    for (u32 k = 0; k < 6; ++k) {
        CellFace face;
        face.other = kBoxSite + k;
        face.plane = PlaneKey(face.other, face.other);
        face.loop.assign(loops[k], loops[k] + 4);
        cell.faces.push_back(std::move(face));
    }
    return cell;
}

void ClipConvex(ConvexCell& cell, const HalfSpace& plane, u64 key, u32 other, f64 epsilon) {
    const u32 count = static_cast<u32>(cell.vertices.size());
    std::vector<f64> d(count);
    bool anyOut = false;
    bool anyIn = false;
    for (u32 i = 0; i < count; ++i) {
        d[i] = plane.distance(cell.vertices[i]);
        anyOut = anyOut || d[i] > epsilon;
        anyIn = anyIn || d[i] < -epsilon;
    }
    if (!anyOut) {
        return;
    }
    if (!anyIn) {
        cell.vertices.clear();
        cell.sites.clear();
        cell.faces.clear();
        return;
    }

    std::unordered_map<u64, u32> made;
    const auto edgeVertex = [&](u32 a, u32 b) {
        const u64 edge = (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
        const auto found = made.find(edge);
        if (found != made.end()) {
            return found->second;
        }
        const f64 t = d[a] / (d[a] - d[b]);
        const Vector3d& pa = cell.vertices[a];
        const Vector3d& pb = cell.vertices[b];
        cell.vertices.push_back(pa + Times(pb - pa, t));
        cell.sites.push_back(EdgeSites(cell.sites[a], cell.sites[b], other));
        d.push_back(0.0);
        const u32 index = static_cast<u32>(cell.vertices.size() - 1);
        made.emplace(edge, index);
        return index;
    };

    std::vector<CellFace> faces;
    std::vector<bool> onCap(count, false);
    for (const CellFace& face : cell.faces) {
        CellFace kept;
        kept.plane = face.plane;
        kept.other = face.other;
        const std::size_t n = face.loop.size();
        for (std::size_t i = 0; i < n; ++i) {
            const u32 a = face.loop[i];
            const u32 b = face.loop[(i + 1) % n];
            if (d[a] <= epsilon) {
                kept.loop.push_back(a);
                if (d[a] >= -epsilon) {
                    onCap[a] = true;
                }
            }
            if ((d[a] < -epsilon && d[b] > epsilon) || (d[a] > epsilon && d[b] < -epsilon)) {
                kept.loop.push_back(edgeVertex(a, b));
            }
        }
        if (kept.loop.size() >= 3) {
            faces.push_back(std::move(kept));
        }
    }

    // The cap: every vertex on the plane, in order round it.
    std::vector<u32> cap;
    for (u32 i = 0; i < count; ++i) {
        if (onCap[i]) {
            cap.push_back(i);
        }
    }
    for (u32 i = count; i < cell.vertices.size(); ++i) {
        cap.push_back(i);
    }
    if (cap.size() >= 3) {
        Vector3d centre(0, 0, 0);
        for (u32 v : cap) {
            centre += cell.vertices[v];
        }
        centre = Times(centre, 1.0 / static_cast<f64>(cap.size()));
        const Vector3d& n = plane.normal;
        const Vector3d axis = std::fabs(n.x) < 0.6 ? Vector3d(1, 0, 0) : Vector3d(0, 1, 0);
        Vector3d u = cross(axis, n);
        u = Times(u, 1.0 / std::sqrt(Dot(u, u)));
        const Vector3d v = cross(n, u);
        std::vector<std::pair<f64, u32>> around;
        for (u32 c : cap) {
            const Vector3d r = cell.vertices[c] - centre;
            around.emplace_back(std::atan2(Dot(r, v), Dot(r, u)), c);
        }
        std::sort(around.begin(), around.end());
        CellFace face;
        face.plane = key;
        face.other = other;
        for (const auto& a : around) {
            face.loop.push_back(a.second);
        }
        faces.push_back(std::move(face));
    }

    // Compact.
    std::vector<u32> remap(cell.vertices.size(), kNoSite);
    ConvexCell out;
    out.site = cell.site;
    for (CellFace& face : faces) {
        for (u32& v : face.loop) {
            if (remap[v] == kNoSite) {
                remap[v] = static_cast<u32>(out.vertices.size());
                out.vertices.push_back(cell.vertices[v]);
                out.sites.push_back(cell.sites[v]);
            }
            v = remap[v];
        }
    }
    out.faces = std::move(faces);
    cell = std::move(out);
}

HalfSpace VoronoiDiagram::plane(u64 key) const {
    const u32 a = PlaneLow(key);
    const u32 b = PlaneHigh(key);
    HalfSpace out;
    if (IsBoxSite(a)) {
        const u32 k = a - kBoxSite;
        const u32 axis = k / 2;
        const bool positive = (k & 1) != 0;
        f64 n[3] = {0, 0, 0};
        n[axis] = positive ? 1.0 : -1.0;
        out.normal = Vector3d(n[0], n[1], n[2]);
        const f64 lowAxis = axis == 0 ? low.x : (axis == 1 ? low.y : low.z);
        const f64 highAxis = axis == 0 ? high.x : (axis == 1 ? high.y : high.z);
        out.offset = positive ? highAxis : -lowAxis;
        return out;
    }
    const Vector3d ga = Scale(seeds[a], grain);
    const Vector3d gb = Scale(seeds[b], grain);
    const Vector3d ng = gb - ga;
    const Vector3d middle = Times(ga + gb, 0.5);
    const f64 offset = Dot(ng, middle);
    const Vector3d nm = Scale(ng, grain);
    const f64 length = std::sqrt(Dot(nm, nm));
    if (!(length > 0.0)) {
        return out;
    }
    out.normal = Times(nm, 1.0 / length);
    out.offset = offset / length;
    return out;
}

bool VoronoiDiagram::corner(const std::array<u32, 4>& sites, Vector3d& out) const {
    if (sites[3] == kNoSite || IsBoxSite(sites[0])) {
        return false;
    }
    HalfSpace planes[3];
    for (u32 i = 0; i < 3; ++i) {
        const u32 x = sites[i + 1];
        planes[i] = plane(IsBoxSite(x) ? PlaneKey(x, x) : PlaneKey(sites[0], x));
    }
    const Vector3d c12 = cross(planes[1].normal, planes[2].normal);
    const Vector3d c20 = cross(planes[2].normal, planes[0].normal);
    const Vector3d c01 = cross(planes[0].normal, planes[1].normal);
    const f64 det = Dot(planes[0].normal, c12);
    if (std::fabs(det) < 1e-12) {
        return false;
    }
    out = Times(Times(c12, planes[0].offset) + Times(c20, planes[1].offset) +
                    Times(c01, planes[2].offset),
                1.0 / det);
    return true;
}

VoronoiDiagram VoronoiCells(std::span<const Vector3d> seeds, const Vector3d& low,
                            const Vector3d& high, const VoronoiOptions& options) {
    VoronoiDiagram diagram;
    diagram.seeds.assign(seeds.begin(), seeds.end());
    diagram.low = low;
    diagram.high = high;
    const f64 shrink = options.stretch > 0.0 ? 1.0 / options.stretch : 1.0;
    switch (options.grain) {
    case Grain::X:
        diagram.grain = Vector3d(shrink, 1, 1);
        break;
    case Grain::Y:
        diagram.grain = Vector3d(1, shrink, 1);
        break;
    case Grain::Z:
        diagram.grain = Vector3d(1, 1, shrink);
        break;
    default:
        break;
    }
    const u32 count = static_cast<u32>(seeds.size());
    diagram.cells.resize(count);
    const f64 diagonal = Diagonal(low, high);
    const f64 epsilon = 1e-9 * diagonal;

    Parallel(count, options.threads, [&](std::size_t index) {
        const u32 i = static_cast<u32>(index);
        const Vector3d gi = Scale(diagram.seeds[i], diagram.grain);
        std::vector<std::pair<f64, u32>> order;
        order.reserve(count);
        for (u32 j = 0; j < count; ++j) {
            if (j != i) {
                const Vector3d r = Scale(diagram.seeds[j], diagram.grain) - gi;
                order.emplace_back(Dot(r, r), j);
            }
        }
        std::sort(order.begin(), order.end());
        ConvexCell cell = BoxCell(low, high, i);
        for (const auto& [squared, j] : order) {
            if (squared == 0.0) {
                if (j < i) {
                    cell = ConvexCell{};
                    break;
                }
                continue;
            }
            f64 reach = 0.0;
            for (const Vector3d& v : cell.vertices) {
                const Vector3d r = Scale(v, diagram.grain) - gi;
                reach = std::max(reach, Dot(r, r));
            }
            // |s_j - s_i| > 2 R, squared.
            if (squared > 4.0 * reach) {
                break;
            }
            const u64 key = PlaneKey(i, j);
            HalfSpace plane = diagram.plane(key);
            if (i > j) {
                plane.normal = Times(plane.normal, -1.0);
                plane.offset = -plane.offset;
            }
            ClipConvex(cell, plane, key, j, epsilon);
            if (cell.empty()) {
                break;
            }
        }
        // Every corner from its sites, so the cells round it agree to the bit.
        for (std::size_t v = 0; v < cell.vertices.size(); ++v) {
            Vector3d at;
            if (diagram.corner(cell.sites[v], at)) {
                const Vector3d r = at - cell.vertices[v];
                if (Dot(r, r) <= 1e-12 * diagonal * diagonal) {
                    cell.vertices[v] = at;
                }
            }
        }
        cell.site = i;
        diagram.cells[i] = std::move(cell);
    });
    return diagram;
}

std::vector<Vector3d> InsetConvex(std::span<const HalfSpace> planes, f64 margin,
                                  const Vector3d& low, const Vector3d& high) {
    ConvexCell cell = BoxCell(low, high, kNoSite);
    const f64 epsilon = 1e-9 * Diagonal(low, high);
    for (std::size_t k = 0; k < planes.size() && !cell.empty(); ++k) {
        HalfSpace moved = planes[k];
        moved.offset -= margin;
        ClipConvex(cell, moved, PlaneKey(static_cast<u32>(k), static_cast<u32>(k)),
                   static_cast<u32>(k), epsilon);
    }
    return cell.vertices;
}

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
