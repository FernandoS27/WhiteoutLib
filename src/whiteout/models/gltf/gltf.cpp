// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/gltf/gltf.h"

namespace whiteout {
namespace models {
namespace gltf {

u32 ComponentSize(ComponentType type) {
    switch (type) {
    case ComponentType::I8:
    case ComponentType::U8:
        return 1;
    case ComponentType::I16:
    case ComponentType::U16:
        return 2;
    case ComponentType::U32:
    case ComponentType::F32:
        return 4;
    case ComponentType::None:
        break;
    }
    return 0;
}

u32 TypeComponentCount(AccessorType type) {
    switch (type) {
    case AccessorType::Scalar:
        return 1;
    case AccessorType::Vec2:
        return 2;
    case AccessorType::Vec3:
        return 3;
    case AccessorType::Vec4:
        return 4;
    case AccessorType::Mat2:
        return 4;
    case AccessorType::Mat3:
        return 9;
    case AccessorType::Mat4:
        return 16;
    case AccessorType::Count:
        break;
    }
    return 0;
}

const char* ToString(AccessorType type) {
    switch (type) {
    case AccessorType::Scalar:
        return "SCALAR";
    case AccessorType::Vec2:
        return "VEC2";
    case AccessorType::Vec3:
        return "VEC3";
    case AccessorType::Vec4:
        return "VEC4";
    case AccessorType::Mat2:
        return "MAT2";
    case AccessorType::Mat3:
        return "MAT3";
    case AccessorType::Mat4:
        return "MAT4";
    case AccessorType::Count:
        break;
    }
    return "SCALAR";
}

AccessorType AccessorTypeFromName(const std::string& name) {
    for (u32 i = 0; i < static_cast<u32>(AccessorType::Count); ++i) {
        const AccessorType type = static_cast<AccessorType>(i);
        if (name == ToString(type)) {
            return type;
        }
    }
    return AccessorType::Count;
}

const char* ToString(AlphaMode mode) {
    switch (mode) {
    case AlphaMode::Opaque:
        return "OPAQUE";
    case AlphaMode::Mask:
        return "MASK";
    case AlphaMode::Blend:
        return "BLEND";
    }
    return "OPAQUE";
}

const char* ToString(AnimInterpolation interp) {
    switch (interp) {
    case AnimInterpolation::Linear:
        return "LINEAR";
    case AnimInterpolation::Step:
        return "STEP";
    case AnimInterpolation::CubicSpline:
        return "CUBICSPLINE";
    }
    return "LINEAR";
}

const char* ToString(AnimPath path) {
    switch (path) {
    case AnimPath::Translation:
        return "translation";
    case AnimPath::Rotation:
        return "rotation";
    case AnimPath::Scale:
        return "scale";
    case AnimPath::Weights:
        return "weights";
    case AnimPath::Count:
        break;
    }
    return "translation";
}

u32 Primitive::attribute(const std::string& semantic) const {
    for (const AttributeBinding& binding : attributes) {
        if (binding.semantic == semantic) {
            return binding.accessor;
        }
    }
    return kNone;
}

} // namespace gltf
} // namespace models
} // namespace whiteout
