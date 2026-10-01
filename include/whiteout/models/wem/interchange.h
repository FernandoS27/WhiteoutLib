// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file interchange.h
 * @brief What the DCC-format converters (OBJ, FBX) share: the axis presets,
 *        the length units, a pose request, and the list of images handed to
 *        the host (FBX_OBJ_DESIGN §3, §6, §16).
 *
 * Every basis here is a **signed permutation** of WEM's canonical space (+X
 * forward, +Y left, +Z up): moving components and flipping signs is exact, so
 * positions round-trip bit-for-bit and no `CoordSpace` grows a value.
 */

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "document.h"

namespace whiteout {
namespace models {
namespace wem {

enum class AxisPreset : u8 {
    YUp,    ///< +Y up, model facing +Z: glTF's basis; Maya, Unity, Blender's OBJ default.
    ZUpMax, ///< +Z up, model facing −Y: `CoordSpace::Max`, SC2's art.
    Native, ///< WEM's own: +Z up, facing +X.
};

/// File axis `i` = `sign[i]` × WEM axis `index[i]`.
struct AxisBasis {
    std::array<u8, 3> index{0, 1, 2};
    std::array<f32, 3> sign{1.0f, 1.0f, 1.0f};

    static AxisBasis FromPreset(AxisPreset preset);

    /// From a file's declared axes (axis 0..2, sign ±1 each). The front axis is
    /// the direction the model faces and becomes WEM +X; up becomes +Z; the
    /// coord axis (the model's left) +Y. Nullopt when two declarations name
    /// the same axis.
    static std::optional<AxisBasis> FromDeclaration(u32 up, i32 upSign, u32 front, i32 frontSign,
                                                    u32 coord, i32 coordSign);

    /// −1 for a mirrored (left-handed) file: winding flips with it.
    f32 determinant() const;

    /// Which file axis WEM axis @p wemAxis lands on, and with what sign.
    u32 fileAxisOf(u32 wemAxis) const;
    f32 fileSignOf(u32 wemAxis) const;

    Vector3f toFile(const Vector3f& v) const;
    Vector3f fromFile(const Vector3f& v) const;
    /// Scale factors move with their axes and keep their signs.
    Vector3f scaleToFile(const Vector3f& s) const;
    Vector3f scaleFromFile(const Vector3f& s) const;
    Quaternion toFile(const Quaternion& q) const;
    Quaternion fromFile(const Quaternion& q) const;
    /// Row-vector matrices (`p * M`, translation in row 3) conjugate.
    Matrix44f toFile(const Matrix44f& m) const;
    Matrix44f fromFile(const Matrix44f& m) const;
};

/// A length an interchange file's numbers can be in. Each is measured in
/// centimetres, FBX's `UnitScaleFactor` and one `Generic` unit (§3).
enum class LengthUnit : u8 {
    Millimetre,
    Centimetre,
    Metre,
    Inch,
    Foot,
};

inline constexpr std::array<LengthUnit, 5> kLengthUnits{LengthUnit::Millimetre, LengthUnit::Centimetre,
                                                        LengthUnit::Metre, LengthUnit::Inch,
                                                        LengthUnit::Foot};

/// Centimetres in one @p unit.
f64 CentimetresPer(LengthUnit unit);

/// The unit @p centimetres measures, within a part in ten thousand: a file
/// that declares 2.54 is in inches.
std::optional<LengthUnit> LengthUnitOf(f64 centimetres);

/// "mm", "cm", "m", "in", "ft": the symbol, which is also its command-line name.
const char* LengthUnitSymbol(LengthUnit unit);
std::optional<LengthUnit> LengthUnitFromSymbol(std::string_view symbol);

/// A pose to bake geometry at, for formats with no skeleton.
struct PoseAt {
    u32 clip = kInvalidIndex;
    f32 seconds = 0.0f;
};

enum class ImageChannel : u8 { All, Red, Green, Blue, Alpha };

/// One image the host must write next to (or into) an exported file: which
/// document texture, which of its channels, and the name the file refers to.
struct InterchangeImage {
    u32 texture = kInvalidIndex;
    ImageChannel channel = ImageChannel::All;
    /// The texture is bound as a normal map — the host restates packed
    /// encodings (DXT5nm and the like) before writing...
    bool normalMap = false;
    /// ...unless something also samples it as colour or data, which the
    /// restatement would break.
    bool otherUse = false;
    std::string name;
};

/// Deterministic, collision-free image names for one export:
/// `<stem>.png`, then `<stem>_2.png`, … — the stem from the texture's path.
class InterchangeImageNames {
public:
    explicit InterchangeImageNames(const Document& document) : document_(document) {}

    /// The name for @p texture (one channel of it), recorded on first use.
    std::string nameFor(u32 texture, ImageChannel channel = ImageChannel::All,
                               bool normalMap = false);

    const std::vector<InterchangeImage>& images() const {
        return images_;
    }
    std::vector<InterchangeImage> take() {
        return std::move(images_);
    }

private:
    const Document& document_;
    std::vector<InterchangeImage> images_;
};

} // namespace wem
} // namespace models
} // namespace whiteout
