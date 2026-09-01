// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file material.h
 * @brief `Material` — the common layer, the native layer, and the sync policy
 *        that keeps them honest (WEM v3, design §7.1).
 *
 * The resolution rule, stated once so nothing has to re-derive it:
 *
 * > **A consumer reading profile *p* uses `native` when it is present and
 * > `sync != CommonEdited`. Everything else uses `common`.** An exporter that
 * > finds `CommonEdited` must either re-derive the native block from common (and
 * > say so) or refuse. Silently writing a stale native block is the one behaviour
 * > this design exists to prevent.
 *
 * Which is why `common` is **not** a public member. A rule about what an edit
 * implies cannot be enforced through a field anyone can assign; `MutableCommon()`
 * is the only way to change it and it is the only place `sync` moves. That
 * accessor sets `CommonEdited` from `InSync` *and* from `NativeAuthoritative` —
 * an edit to a lossy view still makes the view the newest truth, and leaving the
 * native block authoritative over an edit the user just made is the
 * silent-staleness bug in mirror form.
 */

#include <string>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/wem/reflect.h>

#include "common.h"
#include "native.h"

namespace whiteout {
namespace models {
namespace wem {

enum class NativeSync : u8 {
    Absent = 0,          ///< No native block: `Generic`, or authored in WEM.
    InSync,              ///< Common was derived from native and neither has been edited.
    NativeAuthoritative, ///< Native is the truth; common is a lossy view of it.
    CommonEdited,        ///< Common was edited after import; native is now stale.
};

const char* ToString(NativeSync sync);

/// @bind methods
class Material {
public:
    std::string name;

    // --- the common layer --------------------------------------------------

    const CommonMaterial& Common() const {
        return common_;
    }

    /// The editing entry point. Moves `InSync` and `NativeAuthoritative` to
    /// `CommonEdited`; leaves `Absent` alone, since there is no native block to
    /// go stale.
    CommonMaterial& MutableCommon();

    /**
     * @brief Write the common material *without* marking it edited.
     *
     * The importer's path, and only the importer's: this is the call that says
     * "I am deriving common **from** native right now", which is precisely the
     * one edit that does not make native stale. Everything else uses
     * `MutableCommon`.
     */
    CommonMaterial& InitCommon() {
        return common_;
    }

    // --- the native layer --------------------------------------------------

    const NativeMaterial& Native() const {
        return native_;
    }
    NativeSync sync() const {
        return sync_;
    }
    bool hasNative() const {
        return NativeKindOf(native_) != NativeKind::None;
    }
    NativeKind nativeKind() const {
        return NativeKindOf(native_);
    }

    /// Attach @p native and declare the common view faithful. Used by an importer
    /// whose mapping is lossless, and by an exporter that has just re-derived the
    /// block from an edited common.
    void SetNativeInSync(NativeMaterial native);

    /// Attach @p native where common is a lossy projection of it — the usual
    /// import case, and the one that makes `native` the truth.
    void SetNativeAuthoritative(NativeMaterial native);

    /// Drop the block. `DeriveProfile` does this on every cross-family derive,
    /// with a `DroppedNativeBlock` diagnostic each time.
    void ClearNative();

    // --- the resolution rule ------------------------------------------------

    /// §7.1: a consumer reading this material draws from the native block.
    bool NativeIsAuthoritative() const {
        return hasNative() && sync_ != NativeSync::CommonEdited;
    }

    /// An exporter that sees this must re-derive the native block or refuse.
    bool NeedsNativeReDerive() const {
        return hasNative() && sync_ == NativeSync::CommonEdited;
    }

    template <class V>
    void reflect(V& v) {
        // Public because the serializer needs it, and it reaches the private
        // members on purpose: `sync_` is state the setters maintain, and a round
        // trip that reconstructed it from the outside would have to guess.
        v.field("name", name);
        v.field("common", common_);
        v.field("sync", sync_);

        // A native block is skippable by design (§11.3), and that is why it is a
        // `chunk` and not an inline field: a reader without D3 sees the kind
        // byte, consumes a fixed 12-byte reference, and the `ND3_` chunk on the
        // other end survives untouched at the container level. Written inline it
        // would have no length, and the whole material would be unreadable.
        //
        // The kind is computed rather than taken from the variant, because a
        // block this build declined to read leaves the variant empty and the
        // file still has to say what was there.
        NativeKind kind =
            skippedNativeKind_ != NativeKind::None ? skippedNativeKind_ : NativeKindOf(native_);
        v.field("nativeKind", kind);
        switch (kind) {
        case NativeKind::Mdx:
            v.template chunkAlternative<native::MdxMaterial>("mdx", native_, nativeSlot_);
            break;
        case NativeKind::M2:
            v.template chunkAlternative<native::M2Material>("m2", native_, nativeSlot_);
            break;
        case NativeKind::M3:
            v.template chunkAlternative<native::M3Material>("m3", native_, nativeSlot_);
            break;
        case NativeKind::D3:
            v.template chunkAlternative<native::D3Material>("d3", native_, nativeSlot_);
            break;
        case NativeKind::None:
            break;
        }

        if constexpr (V::kReading) {
            // The block was named and did not arrive: this build cannot hold it.
            // `nativeSlot_` is where it still lives, and remembering the pair is
            // what lets the next write point at it instead of dropping it.
            skippedNativeKind_ =
                (kind != NativeKind::None && !hasNative()) ? kind : NativeKind::None;
        }
    }

    /// The native block this build could not read, if there was one. Not the
    /// block -- its bytes are preserved at the container level -- but the two
    /// facts the record itself has to carry: what kind it was, and which
    /// index-table slot it is still in.
    NativeKind skippedNative() const {
        return skippedNativeKind_;
    }

private:
    CommonMaterial common_;
    NativeMaterial native_;
    NativeSync sync_ = NativeSync::Absent;
    NativeKind skippedNativeKind_ = NativeKind::None;
    u32 nativeSlot_ = 0;
};

} // namespace wem
} // namespace models
} // namespace whiteout
