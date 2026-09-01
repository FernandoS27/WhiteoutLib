// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file document.h
 * @brief `Document` — the root (WEM v3, design §9.1).
 *
 * D3 forces the root to hold **several** models. An `.acr` references an `.app`;
 * a trigger event on that actor names *another* `.acr`; a hardpoint attachment is
 * a whole second model. One drawable is not the unit of a file any more.
 *
 * What it does **not** hold is an actor. An actor is a join — a drawable, a look,
 * an animset, and things riding hardpoints — and every one of those is already a
 * WEM concept, so the join is applied at conversion and its result stored (§9.1).
 * Mirroring D3's `.acr` here would be putting a gameplay object in a model format.
 *
 * Models are index-addressed within the document, so an attach point names one
 * with a `u32` and there is no pointer graph to serialize.
 *
 * Serialization is **not** here: §11's reflection-driven reader and writer arrive
 * at P4 and take this struct as their subject. Nothing in this header knows about
 * bytes.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "anim/clip.h"
#include "bounds.h"
#include "materials/texture.h"
#include "model.h"
#include "profile.h"

namespace whiteout {
namespace models {
namespace wem {

/**
 * @brief A chunk this build did not recognise, carried through unchanged (§11.4).
 *
 * Forward compatibility is the point: a newer writer's chunk survives a
 * round-trip through an older reader instead of being silently dropped.
 */
struct UnknownChunk {
    u32 tag = 0;
    u32 version = 0;

    /// The element count from the source index entry. The reader cannot derive
    /// it -- it does not know the element type -- and a reader that *does* know
    /// the type needs it, so it is carried rather than recomputed.
    u32 count = 0;

    /// The index-table slot the chunk occupied. **Load-bearing.** A preserved
    /// chunk's bytes contain `Reference`s of their own, and a `Reference` names
    /// an index-table *slot*; renumbering the table would silently break every
    /// one of them. The writer holds these slots open so the numbering survives.
    u32 index = 0;

    std::vector<u8> data;

    /// Not reflected: an unknown chunk is re-emitted as bytes under its own
    /// tag by the writer, not visited as a record. Reflecting it would mean
    /// claiming to understand it.
};

/// @bind methods
struct Document {
    /// The declared set. A `ProfileMaterialSet` for an undeclared profile is a
    /// structural error, as is a `defaultProfile` outside this list.
    std::vector<ProfileId> profiles;
    ProfileId defaultProfile = ProfileId::Generic;

    /// Canonical, always (§6.4). The per-profile source space lives in the
    /// registry, so an exporter knows how to rebase back.
    CoordSpace space = CoordSpace::Blizzard;

    /// What one WEM unit is, if the caller knows. Scale is **not** normalised —
    /// geometry stays in the units it was authored in.
    f32 unitScale = 1.0f;

    std::string name;
    Extent bounds;

    /// One per drawable. A D3 actor converts to `models[0]` and everything its
    /// attach points reach follows it (§9.1); every other importer produces one.
    std::vector<Model> models;

    std::vector<TextureRef> textures;

    /// Every clip in the document, each naming the model it drives (§10.8).
    ///
    /// Document-level rather than per model because an `.m3a` merge and a D3
    /// anim set both address clips across models, and because a clip index in an
    /// `AnimSet` would otherwise have to carry a model index beside it.
    std::vector<Clip> clips;

    /// The named (tag -> clip) maps. D3 is the only importer that fills this
    /// today; `Model::animSet` is how a model says which one is its default.
    std::vector<AnimSet> animSets;

    /// Chunks this build did not understand, carried through unchanged (§11.4).
    ///
    /// The parser fills it and the writer reads it, so a read-edit-write round
    /// trip preserves them by doing nothing. Clearing it is how a caller drops
    /// them -- an act, not an omission.
    std::vector<UnknownChunk> unknownChunks;

    bool carries(ProfileId profile) const;

    /// Declares @p profile if it is not already declared. Does not create material
    /// sets — that is `AddProfileFromImport` or `DeriveProfile` (§6.6).
    void declare(ProfileId profile);

    /// Every declared profile as a mask.
    ProfileMask declaredMask() const;

    /// `unknownChunks` is deliberately absent from the reflected form: the
    /// chunks live at the *container* level, where their index entries are, and
    /// writing them inside the root record would give them a second home in the
    /// file as well as a second set of slot numbers.
    template <class V>
    void reflect(V& v) {
        v.inlineList("profiles", profiles);
        v.field("defaultProfile", defaultProfile);
        v.field("space", space);
        v.field("unitScale", unitScale);
        v.field("name", name);
        v.field("bounds", bounds);
        v.field("models", models);
        v.field("textures", textures);
        v.field("clips", clips);
        v.field("animSets", animSets);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
