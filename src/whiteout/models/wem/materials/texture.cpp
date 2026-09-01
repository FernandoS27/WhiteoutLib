// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/materials/texture.h>

namespace whiteout {
namespace models {
namespace wem {

namespace {

void appendUnsigned(std::string& out, u32 value) {
    char buffer[16];
    int length = 0;
    if (value == 0) {
        buffer[length++] = '0';
    }
    while (value != 0) {
        buffer[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (length > 0) {
        out.push_back(buffer[--length]);
    }
}

} // namespace

const char* ToString(ColorSpace space) {
    switch (space) {
    case ColorSpace::Auto:
        return "auto";
    case ColorSpace::Linear:
        return "linear";
    case ColorSpace::Srgb:
        return "srgb";
    }
    return "invalid";
}

const char* ToString(WrapMode mode) {
    switch (mode) {
    case WrapMode::Repeat:
        return "repeat";
    case WrapMode::Clamp:
        return "clamp";
    case WrapMode::Mirror:
        return "mirror";
    }
    return "invalid";
}

const char* ToString(TextureKeyKind kind) {
    switch (kind) {
    case TextureKeyKind::None:
        return "none";
    case TextureKeyKind::Path:
        return "path";
    case TextureKeyKind::FileDataId:
        return "fdid";
    case TextureKeyKind::SnoId:
        return "sno";
    }
    return "invalid";
}

std::string Describe(const TextureRef& texture) {
    std::string out;
    switch (KeyKind(texture.key)) {
    case TextureKeyKind::Path:
        return std::get<TexturePath>(texture.key).value;
    case TextureKeyKind::FileDataId:
        out = "fdid:";
        appendUnsigned(out, std::get<TextureFileDataId>(texture.key).value);
        return out;
    case TextureKeyKind::SnoId: {
        const TextureSnoId& sno = std::get<TextureSnoId>(texture.key);
        out = "sno:";
        appendUnsigned(out, sno.group);
        out.push_back(':');
        appendUnsigned(out, sno.id);
        return out;
    }
    case TextureKeyKind::None:
        break;
    }
    // No key, but a path is still a name a human can act on.
    return texture.path;
}

} // namespace wem
} // namespace models
} // namespace whiteout
