// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "extract.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "patch_map.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

QuantizeProblem BuildQuantizeProblem(const Layout& layout, const std::vector<f64>& targets, i32 lower,
                                     f64 mismatch) {
    QuantizeProblem problem;
    const u32 patches = static_cast<u32>(layout.patches.size());
    problem.nodeCount = 2 * patches + 1;
    problem.freeNode = 2 * patches;
    // A side of a rectangle takes the flow in at its first two sides and gives
    // it out at the other two, so opposite sides carry the same count.
    auto end = [&](u32 patch, u8 side, u32& node, bool& head) {
        if (patch != kNone && layout.patches[patch].rectangle) {
            node = 2 * patch + side % 2;
            head = side < 2;
        } else {
            node = problem.freeNode;
            head = true;
        }
    };
    for (u32 a = 0; a < layout.arcs.size(); ++a) {
        const LayoutArc& arc = layout.arcs[a];
        QuantizeArc q;
        end(arc.left, arc.leftSide, q.nodeA, q.headA);
        end(arc.right, arc.rightSide, q.nodeB, q.headB);
        q.target = a < targets.size() ? targets[a] : 1.0;
        q.weight = 1.0 / std::max(q.target, 1.0);
        q.lower = lower;
        problem.arcs.push_back(q);
    }
    if (mismatch > 0.0) {
        for (u32 patch = 0; patch < patches; ++patch) {
            if (!layout.patches[patch].rectangle) {
                continue;
            }
            for (u32 direction = 0; direction < 2; ++direction) {
                // Two heads let the tail sides run longer; two tails, the
                // heads. A unit of the loop is two of difference, so the
                // weight is four times the difference's.
                for (const bool head : {true, false}) {
                    QuantizeArc q;
                    q.nodeA = q.nodeB = 2 * patch + direction;
                    q.headA = q.headB = head;
                    q.target = 0.0;
                    q.weight = 4.0 * mismatch;
                    q.lower = 0;
                    problem.arcs.push_back(q);
                }
            }
        }
    }
    return problem;
}

namespace {

class UnionFind {
public:
    u32 add() {
        parent_.push_back(static_cast<u32>(parent_.size()));
        return parent_.back();
    }
    u32 find(u32 x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }
    /// Joins @p other's set to @p keep's, whose root stays the root.
    bool join(u32 keep, u32 other) {
        keep = find(keep);
        other = find(other);
        if (keep == other) {
            return false;
        }
        parent_[other] = keep;
        return true;
    }

private:
    std::vector<u32> parent_;
};

class Extractor {
public:
    Extractor(const WorkMesh& mesh, const Surface& surface, const Layout& layout, const std::vector<i32>& lengths,
              ExtractStats& stats)
        : mesh_(mesh), surface_(surface), layout_(layout), lengths_(lengths), stats_(stats) {}

    QuadMesh run();
    std::vector<u32> zeroArcVertex;

private:
    u32 addVertex(const V3& position, const SurfacePoint& home, VertexKind kind, const FeaturePoint& onCurve,
                  u32 curve) {
        const u32 id = merge_.add();
        node_.push_back(0);
        out_.positions.push_back(position);
        out_.homes.push_back(home);
        out_.kinds.push_back(kind);
        out_.onCurve.push_back(onCurve);
        out_.curve.push_back(curve);
        return id;
    }
    /// Squeezes @p a and @p b into one point, which keeps the place of a
    /// corner before a feature point, of either before a node, and of a node
    /// before an arc's point.
    bool merge(u32 a, u32 b);
    u32 nodeVertex(u32 workVertex);
    void arcPoints(u32 arc);
    u32 sidePoint(const LayoutPatch& patch, u32 side, i32 at);
    /// Work edge (from << 32 | to) -> its arc, and whether it runs the arc's way.
    const std::unordered_map<u64, std::pair<u32, bool>>& arcOfEdge();
    bool fillPatch(u32 patch);
    /// A disk the layout could not make a rectangle: a fan of triangles from
    /// its middle to its border's arc points. False for any other patch.
    bool fanPatch(u32 patch);
    void keepTriangles(u32 patch);
    void addFace(std::span<const u32> corners, u32 patch, u32 region);
    /// The region a patch lies in: features bound patches, so any triangle's.
    u32 patchRegion(const LayoutPatch& patch) const {
        return patch.triangles.empty() ? kNone : mesh_.regions[patch.triangles.front()];
    }

    const WorkMesh& mesh_;
    const Surface& surface_;
    const Layout& layout_;
    const std::vector<i32>& lengths_;
    ExtractStats& stats_;
    QuadMesh out_;
    UnionFind merge_;
    std::vector<u8> node_; ///< Per output vertex: a layout node's.
    std::unordered_map<u32, u32> nodeOut_; ///< Work vertex -> output vertex.
    std::unordered_map<u64, std::pair<u32, bool>> arcOfEdge_;
    std::vector<std::vector<u32>> arcOut_; ///< Per arc, its inside points in its own order.
    std::vector<std::vector<u32>> rawFaces_;
    std::vector<u32> rawPatch_;
    std::vector<u32> rawRegion_;
};

u32 Extractor::nodeVertex(u32 v) {
    const auto it = nodeOut_.find(v);
    if (it != nodeOut_.end()) {
        return it->second;
    }
    const u32 id = addVertex(mesh_.positions[v], mesh_.homes[v], mesh_.kinds[v], mesh_.onCurve[v], mesh_.curve[v]);
    node_[id] = 1;
    nodeOut_[v] = id;
    return id;
}

bool Extractor::merge(u32 a, u32 b) {
    a = merge_.find(a);
    b = merge_.find(b);
    auto rank = [&](u32 v) {
        const u32 kind = out_.kinds[v] == VertexKind::Corner ? 2 : out_.kinds[v] == VertexKind::Feature ? 1 : 0;
        return 2 * kind + node_[v];
    };
    const bool keepA = rank(a) != rank(b) ? rank(a) > rank(b) : a < b;
    return keepA ? merge_.join(a, b) : merge_.join(b, a);
}

const std::unordered_map<u64, std::pair<u32, bool>>& Extractor::arcOfEdge() {
    if (arcOfEdge_.empty()) {
        for (u32 a = 0; a < layout_.arcs.size(); ++a) {
            const std::vector<u32>& vertices = layout_.arcs[a].vertices;
            for (std::size_t k = 1; k < vertices.size(); ++k) {
                arcOfEdge_[(static_cast<u64>(vertices[k - 1]) << 32) | vertices[k]] = {a, true};
                arcOfEdge_[(static_cast<u64>(vertices[k]) << 32) | vertices[k - 1]] = {a, false};
            }
        }
    }
    return arcOfEdge_;
}

void Extractor::arcPoints(u32 a) {
    const LayoutArc& arc = layout_.arcs[a];
    const i32 length = lengths_[a];
    std::vector<u32>& points = arcOut_[a];
    points.clear();
    if (length < 2 || arc.vertices.size() < 2) {
        return;
    }
    std::vector<f64> at(arc.vertices.size(), 0.0);
    for (std::size_t i = 1; i < arc.vertices.size(); ++i) {
        at[i] = at[i - 1] + Distance(mesh_.positions[arc.vertices[i - 1]], mesh_.positions[arc.vertices[i]]);
    }
    const f64 total = at.back();
    std::size_t segment = 0;
    for (i32 k = 1; k < length; ++k) {
        const f64 want = total * k / length;
        while (segment + 2 < at.size() && at[segment + 1] < want) {
            ++segment;
        }
        const u32 a0 = arc.vertices[segment];
        const u32 a1 = arc.vertices[segment + 1];
        const f64 span = at[segment + 1] - at[segment];
        const f64 t = span > 0.0 ? std::clamp((want - at[segment]) / span, 0.0, 1.0) : 0.0;
        const V3 point = Lerp(mesh_.positions[a0], mesh_.positions[a1], t);
        u32 h = mesh_.find(a0, a1);
        if (h == kNone) {
            h = mesh_.find(a1, a0);
        }
        const f64 reach = Distance(mesh_.positions[a0], mesh_.positions[a1]) + 1e-12;
        if (h != kNone && mesh_.isFeature(h)) {
            // On a feature: exactly on its source curve, and free to slide on it.
            const u32 curve = mesh_.curves[h];
            FeaturePoint start = CurveStart(mesh_, surface_, a0, curve, point);
            if (!start.valid()) {
                start = CurveStart(mesh_, surface_, a1, curve, point);
            }
            if (start.valid()) {
                const FeaturePoint fp = surface_.locateOnCurve(start, point, reach);
                points.push_back(addVertex(surface_.position(fp), surface_.toSurface(fp), VertexKind::Feature, fp, curve));
                continue;
            }
        }
        const u32 start[2] = {mesh_.homes[a0].triangle, mesh_.homes[a1].triangle};
        const LocateFilter filter{h != kNone ? mesh_.regions[h / 3] : kNone,
                                  AgreedNormal(surface_.normal(mesh_.homes[a0]) + surface_.normal(mesh_.homes[a1]), 2)};
        SurfacePoint home = surface_.locate(start, point, reach, filter);
        if (!home.valid()) {
            home = surface_.locate(start, point, reach);
        }
        points.push_back(addVertex(surface_.position(home), home, VertexKind::Free, {}, kNone));
    }
}

u32 Extractor::sidePoint(const LayoutPatch& patch, u32 side, i32 at) {
    i32 offset = 0;
    for (const SideArc& run : patch.sides[side]) {
        const LayoutArc& arc = layout_.arcs[run.arc];
        const i32 length = lengths_[run.arc];
        const u32 first = run.forward ? arc.vertices.front() : arc.vertices.back();
        const u32 last = run.forward ? arc.vertices.back() : arc.vertices.front();
        if (at == offset) {
            return nodeVertex(first);
        }
        if (at < offset + length) {
            const i32 q = at - offset;
            const i32 own = run.forward ? q : length - q;
            return arcOut_[run.arc][static_cast<std::size_t>(own - 1)];
        }
        if (at == offset + length) {
            return nodeVertex(last);
        }
        offset += length;
    }
    // Past the side's end: its last node.
    const SideArc& run = patch.sides[side].back();
    const LayoutArc& arc = layout_.arcs[run.arc];
    return nodeVertex(run.forward ? arc.vertices.back() : arc.vertices.front());
}

void Extractor::addFace(std::span<const u32> corners, u32 patch, u32 region) {
    rawFaces_.emplace_back(corners.begin(), corners.end());
    rawPatch_.push_back(patch);
    rawRegion_.push_back(region);
}

bool Extractor::fillPatch(u32 id) {
    const LayoutPatch& patch = layout_.patches[id];
    if (!patch.rectangle) {
        return false;
    }
    i32 side[4] = {0, 0, 0, 0};
    for (u32 s = 0; s < 4; ++s) {
        for (const SideArc& run : patch.sides[s]) {
            side[s] += lengths_[run.arc];
        }
    }
    // Opposite sides may differ where the quantization could not match them:
    // the grid takes the longer of each pair, and neighbouring columns share
    // the shorter side's points, which makes triangles of the faces along it.
    const i32 n = std::max(side[0], side[2]);
    const i32 m = std::max(side[1], side[3]);
    const u32 mismatched = side[0] != side[2] || side[1] != side[3] ? 1 : 0;
    auto along = [](i32 k, i32 grid, i32 length) {
        return grid > 0 ? static_cast<i32>(std::lround(static_cast<f64>(k) * length / grid)) : 0;
    };
    if (n == 0 || m == 0) {
        // Squeezed flat: its two long sides are one line of points.
        ++stats_.patchesCollapsed;
        const u32 first = n == 0 ? 1 : 0; // the sides that still have points
        const i32 count = n == 0 ? m : n;
        for (i32 k = 0; k <= count; ++k) {
            const u32 a = sidePoint(patch, first, along(k, count, side[first]));
            const u32 b = sidePoint(patch, first + 2, along(count - k, count, side[first + 2]));
            if (merge(a, b)) {
                ++stats_.mergedVertices;
            }
        }
        stats_.patchesMismatched += mismatched;
        return true;
    }

    // The border in loop order: each side's vertices spread over its edge of
    // the rectangle by their length along the side.
    const u32 size = static_cast<u32>(patch.loop.size());
    std::vector<P2> border(size, P2{0.0, 0.0});
    u32 position = patch.corners[0];
    for (u32 s = 0; s < 4; ++s) {
        std::vector<u32> vertices;
        for (const SideArc& run : patch.sides[s]) {
            std::vector<u32> arcVertices = layout_.arcs[run.arc].vertices;
            if (!run.forward) {
                std::reverse(arcVertices.begin(), arcVertices.end());
            }
            vertices.insert(vertices.end(), arcVertices.begin(), arcVertices.end() - 1);
        }
        std::vector<f64> at(vertices.size() + 1, 0.0);
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const u32 next = mesh_.to(patch.loop[(position + i) % size]);
            at[i + 1] = at[i] + Distance(mesh_.positions[vertices[i]], mesh_.positions[next]);
        }
        const f64 total = at.back() > 0.0 ? at.back() : 1.0;
        const f64 extent = s % 2 == 0 ? n : m;
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            if (mesh_.from(patch.loop[position % size]) != vertices[i]) {
                return false; // the sides do not walk the loop: no rectangle after all
            }
            const f64 u = extent * at[i] / total;
            switch (s) {
            case 0:
                border[position % size] = {u, 0.0};
                break;
            case 1:
                border[position % size] = {static_cast<f64>(n), u};
                break;
            case 2:
                border[position % size] = {n - u, static_cast<f64>(m)};
                break;
            default:
                border[position % size] = {0.0, m - u};
                break;
            }
            ++position;
        }
    }
    PatchMap map;
    if (!BuildPatchMap(mesh_, patch, border, map)) {
        return false;
    }
    // Inside points, then the grid of faces.
    std::vector<u32> inside(static_cast<std::size_t>((n - 1) * (m - 1)), kNone);
    for (i32 j = 1; j < m; ++j) {
        for (i32 i = 1; i < n; ++i) {
            f64 w[3] = {0.0, 0.0, 0.0};
            const u32 t = map.locate({static_cast<f64>(i), static_cast<f64>(j)}, w);
            if (t == kNone) {
                return false;
            }
            V3 point{0.0, 0.0, 0.0};
            u32 starts[3];
            f64 reach = 0.0;
            LocateFilter filter{patchRegion(patch), V3{0.0, 0.0, 0.0}};
            for (u32 c = 0; c < 3; ++c) {
                const u32 v = map.vertices[map.corners[3 * t + c]];
                point = point + mesh_.positions[v] * w[c];
                starts[c] = mesh_.homes[v].triangle;
                filter.normal = filter.normal + surface_.normal(mesh_.homes[v]);
            }
            for (u32 c = 0; c < 3; ++c) {
                reach = std::max(reach, Distance(point, mesh_.positions[map.vertices[map.corners[3 * t + c]]]));
            }
            filter.normal = AgreedNormal(filter.normal, 3);
            SurfacePoint home = surface_.locate(starts, point, reach + 1e-12, filter);
            if (!home.valid()) {
                home = surface_.locate(starts, point, reach + 1e-12);
            }
            inside[static_cast<std::size_t>((j - 1) * (n - 1) + (i - 1))] =
                addVertex(surface_.position(home), home, VertexKind::Free, {}, kNone);
        }
    }
    auto grid = [&](i32 i, i32 j) {
        if (j == 0) {
            return sidePoint(patch, 0, along(i, n, side[0]));
        }
        if (i == n) {
            return sidePoint(patch, 1, along(j, m, side[1]));
        }
        if (j == m) {
            return sidePoint(patch, 2, along(n - i, n, side[2]));
        }
        if (i == 0) {
            return sidePoint(patch, 3, along(m - j, m, side[3]));
        }
        return inside[static_cast<std::size_t>((j - 1) * (n - 1) + (i - 1))];
    };
    for (i32 j = 0; j < m; ++j) {
        for (i32 i = 0; i < n; ++i) {
            const u32 quad[4] = {grid(i, j), grid(i + 1, j), grid(i + 1, j + 1), grid(i, j + 1)};
            addFace(quad, id, patchRegion(patch));
        }
    }
    ++stats_.patchesFilled;
    stats_.patchesMismatched += mismatched;
    return true;
}

bool Extractor::fanPatch(u32 id) {
    const LayoutPatch& patch = layout_.patches[id];
    PatchMap map;
    if (!BuildPatchMap(mesh_, patch, CircleBorder(mesh_, patch), map)) {
        return false;
    }
    // The border at arc resolution: each arc's start node, then its points.
    const auto& arcs = arcOfEdge();
    std::vector<u32> border;
    for (const u32 h : patch.loop) {
        const u32 from = mesh_.from(h);
        const auto it = arcs.find((static_cast<u64>(from) << 32) | mesh_.to(h));
        if (it == arcs.end()) {
            return false;
        }
        const auto [arc, forward] = it->second;
        const std::vector<u32>& vertices = layout_.arcs[arc].vertices;
        if (from != (forward ? vertices.front() : vertices.back())) {
            continue; // inside an arc already walked
        }
        border.push_back(nodeVertex(from));
        const std::vector<u32>& points = arcOut_[arc];
        if (forward) {
            border.insert(border.end(), points.begin(), points.end());
        } else {
            border.insert(border.end(), points.rbegin(), points.rend());
        }
    }
    if (border.size() < 3) {
        return false;
    }
    // Through the merges a zero-length arc made: a border that touches itself
    // has no fan, and keeps its triangles instead.
    {
        std::vector<u32> roots;
        for (const u32 point : border) {
            const u32 root = merge_.find(point);
            if (roots.empty() || roots.back() != root) {
                roots.push_back(root);
            }
        }
        if (roots.size() > 1 && roots.front() == roots.back()) {
            roots.pop_back();
        }
        std::vector<u32> sorted = roots;
        std::sort(sorted.begin(), sorted.end());
        if (roots.size() < 3 || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            return false;
        }
    }
    f64 w[3] = {0.0, 0.0, 0.0};
    const u32 t = map.locate({0.0, 0.0}, w);
    if (t == kNone) {
        return false;
    }
    V3 point{0.0, 0.0, 0.0};
    u32 starts[3];
    f64 reach = 0.0;
    LocateFilter filter{patchRegion(patch), V3{0.0, 0.0, 0.0}};
    for (u32 c = 0; c < 3; ++c) {
        const u32 v = map.vertices[map.corners[3 * t + c]];
        point = point + mesh_.positions[v] * w[c];
        starts[c] = mesh_.homes[v].triangle;
        filter.normal = filter.normal + surface_.normal(mesh_.homes[v]);
    }
    for (u32 c = 0; c < 3; ++c) {
        reach = std::max(reach, Distance(point, mesh_.positions[map.vertices[map.corners[3 * t + c]]]));
    }
    filter.normal = AgreedNormal(filter.normal, 3);
    SurfacePoint home = surface_.locate(starts, point, reach + 1e-12, filter);
    if (!home.valid()) {
        home = surface_.locate(starts, point, reach + 1e-12);
    }
    const u32 centre = addVertex(surface_.position(home), home, VertexKind::Free, {}, kNone);
    for (std::size_t k = 0; k < border.size(); ++k) {
        const u32 triangle[3] = {border[k], border[(k + 1) % border.size()], centre};
        addFace(triangle, id, patchRegion(patch));
    }
    ++stats_.patchesFanned;
    return true;
}

void Extractor::keepTriangles(u32 id) {
    // A patch the layout could not make a rectangle keeps its work triangles.
    // Its border takes the arcs' own points -- each work vertex on an arc the
    // nearest by length along it -- so the neighbours' sides meet it point
    // for point; a triangle squeezed to an edge on the way goes.
    const LayoutPatch& patch = layout_.patches[id];
    std::unordered_map<u32, u32> local;
    std::vector<u32> walked;
    for (const u32 h : patch.loop) {
        const auto it = arcOfEdge().find((static_cast<u64>(mesh_.from(h)) << 32) | mesh_.to(h));
        if (it == arcOfEdge().end() || std::find(walked.begin(), walked.end(), it->second.first) != walked.end()) {
            continue;
        }
        const u32 a = it->second.first;
        walked.push_back(a);
        const std::vector<u32>& vertices = layout_.arcs[a].vertices;
        const i32 length = lengths_[a];
        std::vector<f64> at(vertices.size(), 0.0);
        for (std::size_t i = 1; i < vertices.size(); ++i) {
            at[i] = at[i - 1] + Distance(mesh_.positions[vertices[i - 1]], mesh_.positions[vertices[i]]);
        }
        for (std::size_t i = 1; i + 1 < vertices.size(); ++i) {
            const u32 v = vertices[i];
            if (layout_.nodeOf[v] != kNone) {
                continue;
            }
            const f64 along = at.back() > 0.0 ? at[i] / at.back() : 0.0;
            const i32 k = std::clamp(static_cast<i32>(std::lround(along * length)), 0, length);
            local[v] = k == 0 ? nodeVertex(vertices.front())
                     : k == length ? nodeVertex(vertices.back())
                                   : arcOut_[a][static_cast<std::size_t>(k - 1)];
        }
    }
    for (u32 t : patch.triangles) {
        u32 corners[3];
        for (u32 i = 0; i < 3; ++i) {
            const u32 v = mesh_.corners[3 * t + i];
            if (layout_.nodeOf[v] != kNone) {
                corners[i] = nodeVertex(v);
                continue;
            }
            auto it = local.find(v);
            if (it == local.end()) {
                it = local.emplace(v, addVertex(mesh_.positions[v], mesh_.homes[v], mesh_.kinds[v], mesh_.onCurve[v],
                                                mesh_.curve[v]))
                         .first;
            }
            corners[i] = it->second;
        }
        addFace(corners, id, mesh_.regions[t]);
    }
}

QuadMesh Extractor::run() {
    arcOut_.resize(layout_.arcs.size());
    for (u32 a = 0; a < layout_.arcs.size(); ++a) {
        arcPoints(a);
        // A zero-length arc squeezes its two nodes into one.
        if (lengths_[a] == 0 && layout_.arcs[a].vertices.size() >= 2) {
            const u32 front = nodeVertex(layout_.arcs[a].vertices.front());
            const u32 back = nodeVertex(layout_.arcs[a].vertices.back());
            if (merge(front, back)) {
                ++stats_.mergedVertices;
            }
        }
    }
    for (u32 id = 0; id < layout_.patches.size(); ++id) {
        if (!fillPatch(id)) {
            ++stats_.patchesSkipped;
            if (!fanPatch(id)) {
                keepTriangles(id);
            }
        }
    }
    // Faces through the merges; a face left with a repeated corner is gone.
    std::vector<u32> remap(out_.positions.size(), kNone);
    QuadMesh result;
    zeroArcVertex.assign(layout_.arcs.size(), kNone);
    auto keep = [&](u32 v) {
        const u32 root = merge_.find(v);
        if (remap[root] == kNone) {
            remap[root] = result.vertexCount();
            result.positions.push_back(out_.positions[root]);
            result.homes.push_back(out_.homes[root]);
            result.kinds.push_back(out_.kinds[root]);
            result.onCurve.push_back(out_.onCurve[root]);
            result.curve.push_back(out_.curve[root]);
        }
        return remap[root];
    };
    for (u32 a = 0; a < layout_.arcs.size(); ++a) {
        if (lengths_[a] == 0 && layout_.arcs[a].vertices.size() >= 2) {
            zeroArcVertex[a] = keep(nodeVertex(layout_.arcs[a].vertices.front()));
        }
    }
    for (std::size_t f = 0; f < rawFaces_.size(); ++f) {
        std::vector<u32> corners;
        for (u32 v : rawFaces_[f]) {
            const u32 r = keep(v);
            if (std::find(corners.begin(), corners.end(), r) == corners.end()) {
                corners.push_back(r);
            }
        }
        if (corners.size() < 3) {
            ++stats_.degenerateFaces;
            continue;
        }
        if (corners.size() != rawFaces_[f].size()) {
            ++stats_.degenerateFaces;
        }
        result.faceVertices.insert(result.faceVertices.end(), corners.begin(), corners.end());
        result.faceOffsets.push_back(static_cast<u32>(result.faceVertices.size()));
        result.facePatch.push_back(rawPatch_[f]);
        result.faceRegion.push_back(rawRegion_[f]);
        stats_.quads += corners.size() == 4 ? 1 : 0;
    }
    return result;
}

} // namespace

QuadMesh ExtractQuads(const WorkMesh& mesh, const Surface& surface, const Layout& layout,
                      const std::vector<i32>& lengths, ExtractStats& stats) {
    Extractor extractor(mesh, surface, layout, lengths, stats);
    return extractor.run();
}

QuadMesh ExtractQuads(const WorkMesh& mesh, const Surface& surface, const Layout& layout,
                      const std::vector<i32>& lengths, ExtractStats& stats, std::vector<u32>& zeroArcVertex) {
    Extractor extractor(mesh, surface, layout, lengths, stats);
    QuadMesh quads = extractor.run();
    zeroArcVertex = extractor.zeroArcVertex;
    return quads;
}

u32 RemoveDoublets(QuadMesh& quads) {
    u32 merged = 0;
    for (bool again = true; again;) {
        again = false;
        std::unordered_map<u64, u32> undirected;
        std::vector<std::vector<u32>> facesOf(quads.vertexCount());
        for (u32 f = 0; f < quads.faceCount(); ++f) {
            const std::span<const u32> face = quads.face(f);
            for (std::size_t i = 0; i < face.size(); ++i) {
                const u32 a = face[i];
                const u32 b = face[(i + 1) % face.size()];
                ++undirected[(static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b)];
                facesOf[a].push_back(f);
            }
        }
        std::vector<u8> border(quads.vertexCount(), 0);
        for (const auto& [key, count] : undirected) {
            if (count == 1) {
                border[static_cast<u32>(key >> 32)] = 1;
                border[static_cast<u32>(key & 0xFFFFFFFFu)] = 1;
            }
        }
        std::vector<std::vector<u32>> faces(quads.faceCount());
        for (u32 f = 0; f < quads.faceCount(); ++f) {
            const std::span<const u32> face = quads.face(f);
            faces[f].assign(face.begin(), face.end());
        }
        std::vector<u8> dead(quads.faceCount(), 0);
        std::vector<u8> changed(quads.faceCount(), 0);
        for (u32 v = 0; v < quads.vertexCount(); ++v) {
            if (border[v] || facesOf[v].size() != 2) {
                continue;
            }
            const u32 f1 = facesOf[v][0];
            const u32 f2 = facesOf[v][1];
            if (f1 == f2 || changed[f1] || changed[f2]) {
                continue;
            }
            auto rotated = [&](u32 f) {
                std::vector<u32> q = faces[f];
                std::rotate(q.begin(), std::find(q.begin(), q.end(), v), q.end());
                return q;
            };
            const std::vector<u32> q1 = rotated(f1); // v, a, b, c
            const std::vector<u32> q2 = rotated(f2); // v, c, d, a
            if (quads.kinds[v] == VertexKind::Free && q1.size() == 4 && q2.size() == 4 && q2[1] == q1[3] &&
                q2[3] == q1[1] && q1[2] != q2[2]) {
                faces[f1] = {q1[1], q1[2], q1[3], q2[2]};
                dead[f2] = 1;
            } else {
                // Not two quads round a free point -- one on a feature curve
                // would leave the curve across the merged quad: the point
                // leaves both faces, which then meet along the edge between
                // its two neighbours; a face left with two corners goes.
                for (const u32 f : {f1, f2}) {
                    faces[f].erase(std::find(faces[f].begin(), faces[f].end(), v));
                    dead[f] = faces[f].size() < 3 ? 1 : 0;
                }
            }
            changed[f1] = changed[f2] = 1;
            ++merged;
            again = true;
        }
        if (!again) {
            break;
        }
        QuadMesh next = quads;
        next.faceOffsets.assign(1, 0);
        next.faceVertices.clear();
        next.facePatch.clear();
        next.faceRegion.clear();
        const bool regions = quads.faceRegion.size() == quads.faceCount();
        for (u32 f = 0; f < faces.size(); ++f) {
            if (dead[f]) {
                continue;
            }
            next.faceVertices.insert(next.faceVertices.end(), faces[f].begin(), faces[f].end());
            next.faceOffsets.push_back(static_cast<u32>(next.faceVertices.size()));
            next.facePatch.push_back(quads.facePatch[f]);
            if (regions) {
                next.faceRegion.push_back(quads.faceRegion[f]);
            }
        }
        quads = std::move(next);
    }
    return merged;
}

void CompactQuads(QuadMesh& quads) {
    std::vector<u32> remap(quads.vertexCount(), kNone);
    QuadMesh next;
    next.faceOffsets = quads.faceOffsets;
    next.facePatch = quads.facePatch;
    next.faceRegion = quads.faceRegion;
    next.faceVertices.reserve(quads.faceVertices.size());
    for (u32 v : quads.faceVertices) {
        if (remap[v] == kNone) {
            remap[v] = static_cast<u32>(next.positions.size());
            next.positions.push_back(quads.positions[v]);
            next.homes.push_back(quads.homes[v]);
            next.kinds.push_back(quads.kinds[v]);
            next.onCurve.push_back(quads.onCurve[v]);
            next.curve.push_back(quads.curve[v]);
        }
        next.faceVertices.push_back(remap[v]);
    }
    quads = std::move(next);
}

QuadValidity CheckQuads(const QuadMesh& quads) {
    QuadValidity validity;
    std::unordered_map<u64, u32> directed;
    std::unordered_map<u64, u32> undirected;
    std::vector<u32> faces(quads.vertexCount(), 0);
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        const std::span<const u32> face = quads.face(f);
        for (std::size_t i = 0; i < face.size(); ++i) {
            for (std::size_t j = i + 1; j < face.size(); ++j) {
                if (face[i] == face[j]) {
                    ++validity.repeated;
                    validity.badVertices.push_back(face[i]);
                }
            }
            const u32 a = face[i];
            const u32 b = face[(i + 1) % face.size()];
            ++faces[a];
            if (++directed[(static_cast<u64>(a) << 32) | b] == 2) {
                ++validity.flippedEdges;
                validity.badVertices.push_back(a);
                validity.badVertices.push_back(b);
            }
            if (++undirected[(static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b)] == 3) {
                ++validity.nonManifoldEdges;
                validity.badVertices.push_back(a);
                validity.badVertices.push_back(b);
            }
        }
    }
    // A vertex on no border edge with one or two faces is a fold or a doublet.
    std::vector<u8> border(quads.vertexCount(), 0);
    for (const auto& [key, count] : undirected) {
        if (count == 1) {
            border[static_cast<u32>(key >> 32)] = 1;
            border[static_cast<u32>(key & 0xFFFFFFFFu)] = 1;
        }
    }
    for (u32 v = 0; v < quads.vertexCount(); ++v) {
        if (!border[v] && faces[v] > 0 && faces[v] < 3) {
            ++validity.thinVertices;
            validity.badVertices.push_back(v);
        }
    }
    return validity;
}

namespace {

bool Folded(const QuadMesh& quads, const Surface& surface, u32 f) {
    const std::span<const u32> face = quads.face(f);
    V3 newell{0.0, 0.0, 0.0};
    V3 up{0.0, 0.0, 0.0};
    for (std::size_t k = 0; k < face.size(); ++k) {
        newell = newell + Cross(quads.positions[face[k]], quads.positions[face[(k + 1) % face.size()]]);
        up = up + surface.normal(quads.homes[face[k]]);
    }
    return Dot(newell, up) < 0.0;
}

} // namespace

namespace {

bool FlatCorners(const QuadMesh& quads, std::span<const u32> face) {
    const V3 origin = ToV3(ToV3f(quads.positions[face[0]]));
    V3 normal{0.0, 0.0, 0.0};
    f64 longest = 0.0;
    f64 reach = 0.0;
    for (std::size_t i = 0; i < face.size(); ++i) {
        const V3 a = ToV3(ToV3f(quads.positions[face[i]])) - origin;
        const V3 b = ToV3(ToV3f(quads.positions[face[(i + 1) % face.size()]])) - origin;
        normal = normal + Cross(a, b);
        longest = std::max(longest, Length(b - a));
        const V3 p = a + origin;
        reach = std::max({reach, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
    }
    const f64 noise = longest * std::max(reach, longest) * 4.0 * static_cast<f64>(std::numeric_limits<f32>::epsilon());
    return Length(normal) <= noise;
}

} // namespace

bool DegenerateFace(const QuadMesh& quads, u32 face) {
    return FlatCorners(quads, quads.face(face));
}

u32 RepairDegenerate(QuadMesh& quads) {
    std::vector<std::vector<u32>> faces(quads.faceCount());
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        const std::span<const u32> face = quads.face(f);
        faces[f].assign(face.begin(), face.end());
    }
    std::vector<u8> gone(faces.size(), 0);
    auto rank = [&](u32 v) {
        return quads.kinds[v] == VertexKind::Corner ? 2u : (quads.kinds[v] == VertexKind::Feature ? 1u : 0u);
    };
    auto onePoint = [&](u32 a, u32 b) {
        const Vector3f p = ToV3f(quads.positions[a]);
        const Vector3f q = ToV3f(quads.positions[b]);
        return p.x == q.x && p.y == q.y && p.z == q.z;
    };
    u32 mended = 0;
    // Two corners of a side of a flat face on one point: the side collapsed,
    // everywhere. A face with area left round them is drawn, and stays.
    for (bool again = true; again;) {
        again = false;
        for (u32 f = 0; f < faces.size() && !again; ++f) {
            if (gone[f] || !FlatCorners(quads, faces[f])) {
                continue;
            }
            for (std::size_t i = 0; i < faces[f].size() && !gone[f]; ++i) {
                const u32 a = faces[f][i];
                const u32 b = faces[f][(i + 1) % faces[f].size()];
                if (a == b || !onePoint(a, b)) {
                    continue;
                }
                const u32 keep = rank(a) >= rank(b) ? a : b;
                const u32 drop = keep == a ? b : a;
                for (u32 g = 0; g < faces.size(); ++g) {
                    if (gone[g]) {
                        continue;
                    }
                    std::vector<u32> next;
                    for (const u32 v : faces[g]) {
                        const u32 w = v == drop ? keep : v;
                        if (next.empty() || next.back() != w) {
                            next.push_back(w);
                        }
                    }
                    while (next.size() > 1 && next.front() == next.back()) {
                        next.pop_back();
                    }
                    faces[g] = std::move(next);
                    gone[g] = faces[g].size() < 3 ? 1 : 0;
                }
                ++mended;
                again = true;
                break;
            }
        }
    }
    // A face flat along a line: its corners go into the face across its
    // longest side, between that side's ends.
    std::unordered_map<u64, u32> sideOf;
    auto directed = [](u32 a, u32 b) { return (static_cast<u64>(a) << 32) | b; };
    for (u32 f = 0; f < faces.size(); ++f) {
        for (std::size_t i = 0; i < faces[f].size() && !gone[f]; ++i) {
            sideOf[directed(faces[f][i], faces[f][(i + 1) % faces[f].size()])] = f;
        }
    }
    for (u32 f = 0; f < faces.size(); ++f) {
        if (gone[f] || !FlatCorners(quads, faces[f])) {
            continue;
        }
        const std::vector<u32> flat = faces[f];
        const std::size_t n = flat.size();
        std::size_t span = 0;
        f64 longest = -1.0;
        for (std::size_t i = 0; i < n; ++i) {
            const f64 d = Distance(quads.positions[flat[i]], quads.positions[flat[(i + 1) % n]]);
            if (d > longest) {
                longest = d;
                span = i;
            }
        }
        const u32 from = flat[span];
        const u32 to = flat[(span + 1) % n];
        const auto across = sideOf.find(directed(to, from));
        if (across == sideOf.end() || gone[across->second] || across->second == f) {
            continue;
        }
        const u32 hostFace = across->second;
        std::vector<u32>& host = faces[hostFace];
        const auto at = std::find(host.begin(), host.end(), to);
        if (at == host.end() || host[static_cast<std::size_t>(at - host.begin() + 1) % host.size()] != from) {
            continue;
        }
        std::vector<u32> inserted;
        for (std::size_t k = 2; k < n; ++k) {
            inserted.push_back(flat[(span + k) % n]);
        }
        if (std::any_of(inserted.begin(), inserted.end(),
                        [&](u32 v) { return std::find(host.begin(), host.end(), v) != host.end(); })) {
            continue;
        }
        host.insert(at + 1, inserted.begin(), inserted.end());
        gone[f] = 1;
        ++mended;
        for (std::size_t i = 0; i < host.size(); ++i) {
            sideOf[directed(host[i], host[(i + 1) % host.size()])] = hostFace;
        }
    }
    if (mended == 0) {
        return 0;
    }
    QuadMesh next = quads;
    next.faceOffsets.assign(1, 0);
    next.faceVertices.clear();
    next.facePatch.clear();
    next.faceRegion.clear();
    const bool regions = quads.faceRegion.size() == quads.faceCount();
    for (u32 f = 0; f < faces.size(); ++f) {
        if (gone[f]) {
            continue;
        }
        next.faceVertices.insert(next.faceVertices.end(), faces[f].begin(), faces[f].end());
        next.faceOffsets.push_back(static_cast<u32>(next.faceVertices.size()));
        next.facePatch.push_back(quads.facePatch[f]);
        if (regions) {
            next.faceRegion.push_back(quads.faceRegion[f]);
        }
    }
    quads = std::move(next);
    return mended;
}

u32 CountFolded(const QuadMesh& quads, const Surface& surface) {
    u32 folded = 0;
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        folded += Folded(quads, surface, f) ? 1 : 0;
    }
    return folded;
}

u32 UnfoldQuads(QuadMesh& quads, const Surface& surface, u32 rounds) {
    u32 folded = CountFolded(quads, surface);
    std::vector<u8> movable(quads.vertexCount(), 0);
    for (u32 round = 0; round < rounds && folded > 0; ++round) {
        std::fill(movable.begin(), movable.end(), 0);
        std::vector<u8> near(quads.vertexCount(), 0);
        for (u32 f = 0; f < quads.faceCount(); ++f) {
            if (Folded(quads, surface, f)) {
                for (const u32 v : quads.face(f)) {
                    near[v] = 1;
                }
            }
        }
        // The folded faces' corners and every corner of a face beside them.
        for (u32 f = 0; f < quads.faceCount(); ++f) {
            const std::span<const u32> face = quads.face(f);
            if (std::any_of(face.begin(), face.end(), [&](u32 v) { return near[v] != 0; })) {
                for (const u32 v : face) {
                    movable[v] = 1;
                }
            }
        }
        RelaxQuads(quads, surface, 4, 1.0, movable);
        const u32 left = CountFolded(quads, surface);
        if (left >= folded) {
            break;
        }
        folded = left;
    }
    return folded;
}

void RelaxQuads(QuadMesh& quads, const Surface& surface, u32 iterations, f64 step, std::span<const u8> movable) {
    const u32 vertices = quads.vertexCount();
    std::vector<std::vector<u32>> neighbours(vertices);
    std::vector<std::vector<u32>> faces(vertices);
    for (u32 f = 0; f < quads.faceCount(); ++f) {
        const std::span<const u32> face = quads.face(f);
        for (std::size_t i = 0; i < face.size(); ++i) {
            const u32 a = face[i];
            const u32 b = face[(i + 1) % face.size()];
            neighbours[a].push_back(b);
            neighbours[b].push_back(a);
            faces[a].push_back(f);
        }
    }
    for (std::vector<u32>& list : neighbours) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    // A free vertex's faces' one region, which its moves stay in.
    std::vector<u32> region(vertices, kNone);
    if (quads.faceRegion.size() == quads.faceCount()) {
        for (u32 v = 0; v < vertices; ++v) {
            for (const u32 f : faces[v]) {
                const u32 r = quads.faceRegion[f];
                region[v] = f == faces[v].front() ? r : (region[v] == r ? r : kNone);
            }
        }
    }
    auto faceNormal = [&](u32 f, u32 moved, const V3& at) {
        // Newell's normal, with one corner moved.
        const std::span<const u32> face = quads.face(f);
        V3 n{0.0, 0.0, 0.0};
        for (std::size_t i = 0; i < face.size(); ++i) {
            const V3& a = face[i] == moved ? at : quads.positions[face[i]];
            const u32 next = face[(i + 1) % face.size()];
            const V3& b = next == moved ? at : quads.positions[next];
            n = n + Cross(a, b);
        }
        return n;
    };
    // Corners that turn the wrong way round their face: a dart, which a move
    // may take away but never add.
    auto reflex = [&](u32 f, u32 moved, const V3& at) {
        const std::span<const u32> face = quads.face(f);
        const V3 n = faceNormal(f, moved, at);
        auto point = [&](u32 k) {
            const u32 w = face[k % face.size()];
            return w == moved ? at : quads.positions[w];
        };
        u32 count = 0;
        for (std::size_t i = 0; i < face.size(); ++i) {
            const V3 here = point(i);
            const V3 out = point(i + 1) - here;
            const V3 in = point(i + face.size() - 1) - here;
            count += Dot(Cross(out, in), n) < 0.0 ? 1 : 0;
        }
        return count;
    };
    for (u32 iteration = 0; iteration < iterations; ++iteration) {
        for (u32 v = 0; v < vertices; ++v) {
            if (quads.kinds[v] == VertexKind::Corner || neighbours[v].empty() || (!movable.empty() && !movable[v])) {
                continue;
            }
            const V3 p = quads.positions[v];
            V3 target = p;
            if (quads.kinds[v] == VertexKind::Feature) {
                V3 sum{0.0, 0.0, 0.0};
                u32 count = 0;
                for (u32 w : neighbours[v]) {
                    const bool along = (quads.kinds[w] == VertexKind::Feature && quads.curve[w] == quads.curve[v]) ||
                                       quads.kinds[w] == VertexKind::Corner;
                    if (along) {
                        sum = sum + quads.positions[w];
                        ++count;
                    }
                }
                if (count != 2) {
                    continue;
                }
                target = sum * 0.5;
            } else {
                V3 sum{0.0, 0.0, 0.0};
                for (u32 w : neighbours[v]) {
                    sum = sum + quads.positions[w];
                }
                const V3 centroid = sum * (1.0 / static_cast<f64>(neighbours[v].size()));
                target = p + Tangent(centroid - p, surface.normal(quads.homes[v]));
            }
            const V3 wanted = p + (target - p) * step;
            const f64 moved = Distance(p, wanted);
            if (moved <= 0.0) {
                continue;
            }
            SurfacePoint home;
            FeaturePoint onCurve;
            V3 placed{0.0, 0.0, 0.0};
            if (quads.kinds[v] == VertexKind::Feature) {
                onCurve = surface.locateOnCurve(quads.onCurve[v], wanted, 2.0 * moved);
                home = surface.toSurface(onCurve);
                placed = surface.position(onCurve);
            } else {
                const u32 start = quads.homes[v].triangle;
                LocateFilter filter = surface.filterAt(quads.homes[v]);
                if (region[v] != kNone) {
                    filter.region = region[v];
                }
                home = surface.locate(std::span<const u32>(&start, 1), wanted, 2.0 * moved, filter);
                placed = home.valid() ? surface.position(home) : p;
            }
            if (!home.valid()) {
                continue;
            }
            // Faces against the surface, before and after: fewer is always
            // taken, which unfolds what extraction left folded; as many, only
            // a move that turns no face over and adds no dart.
            bool folds = false;
            u32 againstBefore = 0;
            u32 againstAfter = 0;
            u32 dartsBefore = 0;
            u32 dartsAfter = 0;
            for (u32 f : faces[v]) {
                const V3 before = faceNormal(f, kNone, p);
                const V3 after = faceNormal(f, v, placed);
                V3 up{0.0, 0.0, 0.0};
                for (const u32 w : quads.face(f)) {
                    up = up + surface.normal(w == v ? home : quads.homes[w]);
                }
                againstBefore += Dot(before, up) <= 0.0 ? 1 : 0;
                againstAfter += Dot(after, up) <= 0.0 ? 1 : 0;
                folds = folds || Dot(before, after) <= 0.0;
                dartsBefore += reflex(f, kNone, p);
                dartsAfter += reflex(f, v, placed);
            }
            if (againstAfter > againstBefore ||
                (againstAfter == againstBefore && (folds || dartsAfter > dartsBefore))) {
                continue;
            }
            quads.positions[v] = placed;
            quads.homes[v] = home;
            if (quads.kinds[v] == VertexKind::Feature) {
                quads.onCurve[v] = onCurve;
            }
        }
    }
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
