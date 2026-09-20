// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/mirror.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

bool IsSeparator(char c) {
    return c == '_' || c == ' ' || c == '.' || c == '-';
}

/// One token of a name: where it starts, how long it is, and whether a
/// separator (rather than a case change) ended the one before it.
struct Token {
    std::size_t begin = 0;
    std::size_t end = 0;
};

/// @p name split at separators and at case changes, the separators left out.
std::vector<Token> Tokens(const std::string& name) {
    std::vector<Token> out;
    std::size_t begin = std::string::npos;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (IsSeparator(name[i])) {
            if (begin != std::string::npos) {
                out.push_back({begin, i});
                begin = std::string::npos;
            }
            continue;
        }
        const bool caseChange = begin != std::string::npos && std::isupper(c) &&
                                std::islower(static_cast<unsigned char>(name[i - 1]));
        if (caseChange) {
            out.push_back({begin, i});
            begin = i;
            continue;
        }
        if (begin == std::string::npos) {
            begin = i;
        }
    }
    if (begin != std::string::npos) {
        out.push_back({begin, name.size()});
    }
    return out;
}

char FlipCase(char sample, char c) {
    return std::isupper(static_cast<unsigned char>(sample))
               ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
               : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool EqualNoCase(const std::string& text, std::size_t begin, std::size_t length,
                 const char* word) {
    const std::size_t wordLength = std::char_traits<char>::length(word);
    if (length != wordLength) {
        return false;
    }
    for (std::size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[begin + i])) !=
            std::tolower(static_cast<unsigned char>(word[i]))) {
            return false;
        }
    }
    return true;
}

/// The left/right pairs, longest first so `Left` beats `L`.
constexpr const char* kSides[][2] = {{"Left", "Right"}, {"Lf", "Rt"}, {"L", "R"}};

/// Swaps the side word in `[begin, begin + length)` of @p name, keeping the
/// case of its first letter. Returns an empty string when it holds none.
std::string SwapSide(const std::string& name, std::size_t begin, std::size_t length) {
    for (const auto& pair : kSides) {
        for (int side = 0; side < 2; ++side) {
            const char* from = pair[side];
            const char* to = pair[1 - side];
            const std::size_t fromLength = std::char_traits<char>::length(from);
            if (length != fromLength || !EqualNoCase(name, begin, length, from)) {
                continue;
            }
            // The case of the word, not of the letters under it: `Left` is
            // four letters and `Right` five, so a per-letter copy turned
            // `LeftHand` into `RighTHand`.
            bool allUpper = true;
            for (std::size_t i = 0; i < length; ++i) {
                allUpper = allUpper && !std::islower(static_cast<unsigned char>(name[begin + i]));
            }
            std::string replacement;
            for (std::size_t i = 0; i < std::char_traits<char>::length(to); ++i) {
                const char sample = allUpper || i == 0 ? name[begin] : 'a';
                replacement.push_back(FlipCase(sample, to[i]));
            }
            std::string out = name;
            out.replace(begin, length, replacement);
            return out;
        }
    }
    return {};
}

} // namespace

std::string MirroredName(const std::string& name) {
    const std::vector<Token> tokens = Tokens(name);
    // A whole token first: `Bone_Arm_L`, `Bip01 L Thigh`, `hand.l`.
    for (const Token& token : tokens) {
        std::string swapped = SwapSide(name, token.begin, token.end - token.begin);
        if (!swapped.empty()) {
            return swapped;
        }
    }
    // Then a prefix that starts a word of its own: `LHand`, `L01`. The case
    // change would have split `LeftHand` already, so what is left here is a
    // side letter run into an upper-case or numbered word.
    //
    // A bare suffix is deliberately NOT a side: `control` ends in an `l` and
    // `Root` starts with an `R`, and neither is a left or a right. A suffix
    // that means one is written with a separator (`hand.l`, `foot_rt`), and a
    // separator makes it a whole token above.
    for (const Token& token : tokens) {
        const std::size_t length = token.end - token.begin;
        for (const auto& pair : kSides) {
            for (int side = 0; side < 2; ++side) {
                const std::size_t wordLength = std::char_traits<char>::length(pair[side]);
                if (wordLength >= length) {
                    continue;
                }
                const unsigned char next =
                    static_cast<unsigned char>(name[token.begin + wordLength]);
                if (!std::isupper(next) && !std::isdigit(next)) {
                    continue;
                }
                const std::string swapped = SwapSide(name, token.begin, wordLength);
                if (!swapped.empty()) {
                    return swapped;
                }
            }
        }
    }
    return {};
}

namespace {

f32 Axis(const Vector3f& p, MirrorAxis axis) {
    switch (axis) {
    case MirrorAxis::X:
        return p.x;
    case MirrorAxis::Y:
        return p.y;
    case MirrorAxis::Z:
        return p.z;
    }
    return p.y;
}

Vector3f Mirrored(const Vector3f& p, MirrorAxis axis) {
    Vector3f out = p;
    switch (axis) {
    case MirrorAxis::X:
        out.x = -out.x;
        break;
    case MirrorAxis::Y:
        out.y = -out.y;
        break;
    case MirrorAxis::Z:
        out.z = -out.z;
        break;
    }
    return out;
}

f32 DistanceSquared(const Vector3f& a, const Vector3f& b) {
    const f32 dx = a.x - b.x;
    const f32 dy = a.y - b.y;
    const f32 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

std::vector<BoneMirror> BuildBoneMirror(const NodeTree& nodes, MirrorAxis axis) {
    const u32 count = nodes.size();
    std::vector<BoneMirror> out(count);
    std::vector<Vector3f> pivots(count);
    std::unordered_map<std::string, u32> byName;
    f32 size = 0.0f;
    for (u32 n = 0; n < count; ++n) {
        const Matrix44f bind = Matrix44f::inverse(nodes.inverseBindMatrix(n));
        pivots[n] = Vector3f{bind.data[3][0], bind.data[3][1], bind.data[3][2]};
        byName.emplace(nodes.nodes[n].name, n);
        size = std::max({size, std::abs(pivots[n].x), std::abs(pivots[n].y),
                         std::abs(pivots[n].z)});
    }
    const f32 tolerance = std::max(size * 0.01f, 1e-4f);

    for (u32 n = 0; n < count; ++n) {
        const Node& node = nodes.nodes[n];
        // 0. The user's own answer, saved on the bone (§13.4).
        if (node.skin.mirror != kInvalidNode && node.skin.mirror < count) {
            out[n] = {node.skin.mirror, MirrorSource::Override};
            continue;
        }
        // 1. By name.
        const std::string mirrored = MirroredName(node.name);
        if (!mirrored.empty()) {
            const auto found = byName.find(mirrored);
            if (found != byName.end() && found->second != n) {
                out[n] = {found->second, MirrorSource::Name};
                continue;
            }
        }
        // 3. A bone on the plane is its own mirror -- checked before the search,
        // because the nearest pivot to its own mirrored one is itself anyway.
        if (std::abs(Axis(pivots[n], axis)) <= tolerance) {
            out[n] = {n, MirrorSource::Self};
            continue;
        }
        // 2. By position.
        const Vector3f want = Mirrored(pivots[n], axis);
        u32 best = kInvalidNode;
        f32 bestDistance = tolerance * tolerance;
        for (u32 other = 0; other < count; ++other) {
            if (other == n || nodes.nodes[other].kind != node.kind) {
                continue;
            }
            const f32 distance = DistanceSquared(pivots[other], want);
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = other;
            }
        }
        if (best != kInvalidNode) {
            out[n] = {best, MirrorSource::Position};
        }
    }
    return out;
}

std::vector<u32> BuildPointMirror(const Mesh& mesh, const PointTable& points, MirrorAxis axis) {
    std::vector<u32> out(points.pointCount, kInvalidIndex);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    if (points.pointCount == 0 || positions.empty()) {
        return out;
    }
    // A point's position is its first member's: every member shares one to
    // within the weld tolerance.
    std::vector<Vector3f> at(points.pointCount);
    for (u32 p = 0; p < points.pointCount; ++p) {
        const std::span<const u32> members = points.membersOf(p);
        at[p] = !members.empty() && members[0] < positions.size() ? positions[members[0]]
                                                                  : Vector3f{0, 0, 0};
    }
    const f32 tolerance = points.weldTolerance > 0.0f ? points.weldTolerance * 100.0f : 1e-3f;

    // The same hash grid the table welds with, at the mirror's own tolerance.
    const f32 cell = std::max(tolerance, 1e-6f);
    struct Key {
        i64 x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key& key) const {
            std::size_t h = 1469598103934665603ull;
            for (const i64 word : {key.x, key.y, key.z}) {
                h = (h ^ static_cast<std::size_t>(word)) * 1099511628211ull;
            }
            return h;
        }
    };
    const auto cellOf = [cell](const Vector3f& p) {
        return Key{static_cast<i64>(std::floor(p.x / cell)), static_cast<i64>(std::floor(p.y / cell)),
                   static_cast<i64>(std::floor(p.z / cell))};
    };
    std::unordered_map<Key, std::vector<u32>, Hash> grid;
    grid.reserve(points.pointCount);
    for (u32 p = 0; p < points.pointCount; ++p) {
        grid[cellOf(at[p])].push_back(p);
    }

    for (u32 p = 0; p < points.pointCount; ++p) {
        const Vector3f want = Mirrored(at[p], axis);
        const Key key = cellOf(want);
        u32 best = kInvalidIndex;
        f32 bestDistance = tolerance * tolerance;
        for (i64 dx = -1; dx <= 1; ++dx) {
            for (i64 dy = -1; dy <= 1; ++dy) {
                for (i64 dz = -1; dz <= 1; ++dz) {
                    const auto found = grid.find(Key{key.x + dx, key.y + dy, key.z + dz});
                    if (found == grid.end()) {
                        continue;
                    }
                    for (const u32 other : found->second) {
                        const f32 distance = DistanceSquared(at[other], want);
                        if (distance <= bestDistance) {
                            bestDistance = distance;
                            best = other;
                        }
                    }
                }
            }
        }
        out[p] = best;
    }
    return out;
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout
