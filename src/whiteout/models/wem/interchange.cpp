// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/interchange.h"

#include <cmath>

namespace whiteout {
namespace models {
namespace wem {

AxisBasis AxisBasis::FromPreset(AxisPreset preset) {
    AxisBasis basis;
    switch (preset) {
    case AxisPreset::YUp:
        // file = (y, z, x): glTF's cyclic permutation.
        basis.index = {1, 2, 0};
        break;
    case AxisPreset::ZUpMax:
        // file = (y, −x, z): facing −Y with +X its left.
        basis.index = {1, 0, 2};
        basis.sign = {1.0f, -1.0f, 1.0f};
        break;
    case AxisPreset::Native:
        break;
    }
    return basis;
}

std::optional<AxisBasis> AxisBasis::FromDeclaration(u32 up, i32 upSign, u32 front, i32 frontSign,
                                                    u32 coord, i32 coordSign) {
    if (up > 2 || front > 2 || coord > 2 || up == front || up == coord || front == coord) {
        return std::nullopt;
    }
    AxisBasis basis;
    basis.index[front] = 0;
    basis.sign[front] = frontSign < 0 ? -1.0f : 1.0f;
    basis.index[coord] = 1;
    basis.sign[coord] = coordSign < 0 ? -1.0f : 1.0f;
    basis.index[up] = 2;
    basis.sign[up] = upSign < 0 ? -1.0f : 1.0f;
    return basis;
}

f32 AxisBasis::determinant() const {
    // An odd permutation of the axes reflects; each negated axis does too.
    const bool odd = (index[0] > index[1]) != ((index[0] > index[2]) != (index[1] > index[2]));
    return (odd ? -1.0f : 1.0f) * sign[0] * sign[1] * sign[2];
}

u32 AxisBasis::fileAxisOf(u32 wemAxis) const {
    for (u32 i = 0; i < 3; ++i) {
        if (index[i] == wemAxis) {
            return i;
        }
    }
    return wemAxis;
}

f32 AxisBasis::fileSignOf(u32 wemAxis) const {
    return sign[fileAxisOf(wemAxis)];
}

namespace {

f32 Component(const Vector3f& v, u32 i) {
    return i == 0 ? v.x : i == 1 ? v.y : v.z;
}

void SetComponent(Vector3f& v, u32 i, f32 value) {
    (i == 0 ? v.x : i == 1 ? v.y : v.z) = value;
}

} // namespace

Vector3f AxisBasis::toFile(const Vector3f& v) const {
    return {sign[0] * Component(v, index[0]), sign[1] * Component(v, index[1]),
            sign[2] * Component(v, index[2])};
}

Vector3f AxisBasis::fromFile(const Vector3f& v) const {
    Vector3f out;
    for (u32 i = 0; i < 3; ++i) {
        SetComponent(out, index[i], sign[i] * Component(v, i));
    }
    return out;
}

Vector3f AxisBasis::scaleToFile(const Vector3f& s) const {
    return {Component(s, index[0]), Component(s, index[1]), Component(s, index[2])};
}

Vector3f AxisBasis::scaleFromFile(const Vector3f& s) const {
    Vector3f out;
    for (u32 i = 0; i < 3; ++i) {
        SetComponent(out, index[i], Component(s, i));
    }
    return out;
}

Quaternion AxisBasis::toFile(const Quaternion& q) const {
    // The axis is a pseudo-vector: a reflecting basis flips it as well.
    const Vector3f axis = toFile(Vector3f{q.x, q.y, q.z});
    const f32 det = determinant();
    return {axis.x * det, axis.y * det, axis.z * det, q.w};
}

Quaternion AxisBasis::fromFile(const Quaternion& q) const {
    const Vector3f axis = fromFile(Vector3f{q.x, q.y, q.z});
    const f32 det = determinant();
    return {axis.x * det, axis.y * det, axis.z * det, q.w};
}

Matrix44f AxisBasis::toFile(const Matrix44f& m) const {
    // M_file = Pᵀ · M · P for row vectors, P[index[i]][i] = sign[i].
    Matrix44f out = m;
    for (u32 i = 0; i < 4; ++i) {
        const u32 si = i < 3 ? index[i] : 3;
        const f32 fi = i < 3 ? sign[i] : 1.0f;
        for (u32 j = 0; j < 4; ++j) {
            const u32 sj = j < 3 ? index[j] : 3;
            const f32 fj = j < 3 ? sign[j] : 1.0f;
            out.data[i][j] = fi * fj * m.data[si][sj];
        }
    }
    return out;
}

Matrix44f AxisBasis::fromFile(const Matrix44f& m) const {
    Matrix44f out = m;
    for (u32 i = 0; i < 4; ++i) {
        const u32 si = i < 3 ? index[i] : 3;
        const f32 fi = i < 3 ? sign[i] : 1.0f;
        for (u32 j = 0; j < 4; ++j) {
            const u32 sj = j < 3 ? index[j] : 3;
            const f32 fj = j < 3 ? sign[j] : 1.0f;
            out.data[si][sj] = fi * fj * m.data[i][j];
        }
    }
    return out;
}

// ============================================================================
// LengthUnit
// ============================================================================

f64 CentimetresPer(LengthUnit unit) {
    switch (unit) {
    case LengthUnit::Millimetre:
        return 0.1;
    case LengthUnit::Centimetre:
        return 1.0;
    case LengthUnit::Metre:
        return 100.0;
    case LengthUnit::Inch:
        return 2.54;
    case LengthUnit::Foot:
        return 30.48;
    }
    return 1.0;
}

std::optional<LengthUnit> LengthUnitOf(f64 centimetres) {
    for (const LengthUnit unit : kLengthUnits) {
        const f64 exact = CentimetresPer(unit);
        if (std::abs(centimetres - exact) <= exact * 1e-4) {
            return unit;
        }
    }
    return std::nullopt;
}

const char* LengthUnitSymbol(LengthUnit unit) {
    switch (unit) {
    case LengthUnit::Millimetre:
        return "mm";
    case LengthUnit::Centimetre:
        return "cm";
    case LengthUnit::Metre:
        return "m";
    case LengthUnit::Inch:
        return "in";
    case LengthUnit::Foot:
        return "ft";
    }
    return "cm";
}

std::optional<LengthUnit> LengthUnitFromSymbol(std::string_view symbol) {
    for (const LengthUnit unit : kLengthUnits) {
        if (symbol == LengthUnitSymbol(unit)) {
            return unit;
        }
    }
    return std::nullopt;
}

// ============================================================================
// InterchangeImageNames
// ============================================================================

namespace {

std::string StemOf(const TextureRef& ref, u32 index) {
    std::string base = !ref.path.empty() ? ref.path : Describe(ref);
    const std::size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base = base.substr(0, dot);
    }
    // Portable file names: MTL paths with spaces are poorly read, and FBX
    // readers disagree about non-ASCII.
    for (char& c : base) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            c = '_';
        }
    }
    if (base.empty()) {
        base = "texture_" + std::to_string(index);
    }
    return base;
}

const char* SuffixOf(ImageChannel channel) {
    switch (channel) {
    case ImageChannel::Red:
        return "_r";
    case ImageChannel::Green:
        return "_g";
    case ImageChannel::Blue:
        return "_b";
    case ImageChannel::Alpha:
        return "_a";
    case ImageChannel::All:
        break;
    }
    return "";
}

} // namespace

std::string InterchangeImageNames::nameFor(u32 texture, ImageChannel channel,
                                                  bool normalMap) {
    for (InterchangeImage& image : images_) {
        if (image.texture == texture && image.channel == channel) {
            image.normalMap = image.normalMap || normalMap;
            image.otherUse = image.otherUse || !normalMap;
            return image.name;
        }
    }
    const std::string stem =
        (texture < document_.textures.size() ? StemOf(document_.textures[texture], texture)
                                              : "texture_" + std::to_string(texture)) +
        SuffixOf(channel);
    std::string name = stem + ".png";
    for (u32 attempt = 2;; ++attempt) {
        bool taken = false;
        for (const InterchangeImage& image : images_) {
            if (image.name == name) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            break;
        }
        name = stem + "_" + std::to_string(attempt) + ".png";
    }
    images_.push_back(InterchangeImage{texture, channel, normalMap, !normalMap, std::move(name)});
    return images_.back().name;
}

} // namespace wem
} // namespace models
} // namespace whiteout
