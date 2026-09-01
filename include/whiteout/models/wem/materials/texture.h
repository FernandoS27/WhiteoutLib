// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file texture.h
 * @brief The document-wide texture table (WEM v3, design §7.4).
 *
 * The table is document-wide and shared across profile sets, so a Reforged
 * material and a classic material that use the same file reference one entry.
 *
 * `TextureKey` is a variant because the six profiles genuinely address textures
 * three different ways — WC3 by path, WoW by path *or* fileDataID, SC2 and
 * Heroes by path, D3 by SNO id (which **is** a fileDataID in all but name).
 * Three parallel fields plus a convention about which one is set is how that
 * goes wrong.
 *
 * A CASC fileDataID read additionally needs a locale mask to pick among the one
 * root entry per locale. That mask belongs to the *provider*, and WEM must not
 * pretend to own it — the key names the file, the host resolves it.
 */

#include <string>
#include <variant>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/wem/reflect.h>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Sampler vocabulary
// ============================================================================

/**
 * @brief How a texture's samples are to be interpreted.
 *
 * `Auto` is resolved from the **slot** the texture is used in, not from its
 * filename: the filename guess misses 22.7% of `.m3` normal maps, and an sRGB
 * view on a DXT5nm map mirrors the lighting. `AutoColorSpaceFor` in common.h is
 * the resolution, one overload per slot vocabulary.
 */
enum class ColorSpace : u8 {
    Auto,   ///< Decide from the slot (`AutoColorSpaceFor`).
    Linear, ///< Data: normals, masks, ORM, roughness.
    Srgb,   ///< Colour: base colour, emissive, diffuse.
};

const char* ToString(ColorSpace space);

enum class WrapMode : u8 {
    Repeat,
    Clamp,
    Mirror,
};

const char* ToString(WrapMode mode);

// ============================================================================
// TextureKey
// ============================================================================

/// Addressed by name — WC3, SC2, Heroes, and WoW's pre-Legion content.
struct TexturePath {
    std::string value;

    template <class V>
    void reflect(V& v) {
        v.field("value", value);
    }
};

/// Addressed by CASC fileDataID — WoW. The locale mask is the provider's.
struct TextureFileDataId {
    u32 value = 0;

    template <class V>
    void reflect(V& v) {
        v.field("value", value);
    }
};

/// Addressed by SNO id — Diablo III. `group` is the SNO group (44 for textures).
struct TextureSnoId {
    u32 group = 0;
    u32 id = 0;

    template <class V>
    void reflect(V& v) {
        v.field("group", group);
        v.field("id", id);
    }
};

using TextureKey = std::variant<std::monostate, TexturePath, TextureFileDataId, TextureSnoId>;

enum class TextureKeyKind : u8 {
    None = 0, ///< The reference names nothing yet.
    Path = 1,
    FileDataId = 2,
    SnoId = 3,
};

const char* ToString(TextureKeyKind kind);

inline TextureKeyKind KeyKind(const TextureKey& key) {
    return static_cast<TextureKeyKind>(key.index());
}

// ============================================================================
// TextureRef
// ============================================================================

/**
 * @brief One entry of `Document::textures`.
 *
 * `path` is kept alongside an id key when the source knew both: a fileDataID is
 * what resolves, and the path is what a human reads in a diagnostic.
 */
struct TextureRef {
    TextureKey key;
    std::string path;
    u32 flags = 0;         ///< Format-scoped wrap/filter bits the converter kept.
    u32 replaceableId = 0; ///< WC3 replaceable texture id; WoW's replaceable slot.
    u8 slotType = 0;       ///< WoW texture type 0..26; the 4th replaceable fills type 5.
    ColorSpace declaredSpace = ColorSpace::Auto;

    template <class V>
    void reflect(V& v) {
        // The key is a variant, so its discriminator is a field of its own and
        // the alternative follows. `None` writes nothing after the byte.
        const TextureKeyKind kind = VariantKind<TextureKeyKind>(v, "keyKind", key);
        switch (kind) {
        case TextureKeyKind::Path:
            v.field("key", VariantAs<TexturePath>(key));
            break;
        case TextureKeyKind::FileDataId:
            v.field("key", VariantAs<TextureFileDataId>(key));
            break;
        case TextureKeyKind::SnoId:
            v.field("key", VariantAs<TextureSnoId>(key));
            break;
        case TextureKeyKind::None:
            break;
        }
        v.field("path", path);
        v.field("flags", flags);
        v.field("replaceableId", replaceableId);
        v.field("slotType", slotType);
        v.field("declaredSpace", declaredSpace);
    }

    bool empty() const {
        return KeyKind(key) == TextureKeyKind::None && path.empty();
    }
};

/// "path/to.blp" / "fdid:1234567" / "sno:44:8921" / "" — for diagnostics.
std::string Describe(const TextureRef& texture);

} // namespace wem
} // namespace models
} // namespace whiteout
