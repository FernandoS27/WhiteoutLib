// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pipeline.h
/// @brief Mipmap generation pipeline: pre-process → downsample → post-process.
///
/// A MipmapPipeline chains a configurable sequence of in-place processing
/// stages around a central downsample filter.  Calling `execute()` produces
/// a target-sized mip level from a source image.

#pragma once

#include "mip_image.h"

#include <functional>
#include <vector>

namespace whiteout::textures::mipmap {

/// An in-place image transform (e.g. linearize, renormalize).
using Stage = std::function<void(MipImage&)>;

/// A downsample operation: produces a target-sized image from a source.
/// Parameters: (source image, target width, target height).
using Filter = std::function<MipImage(const MipImage&, u32, u32)>;

/// Configurable pipeline that generates one mip level.
///
/// Pre-process stages are applied to a copy of the source before filtering.
/// The downsample filter then reduces from source resolution to the given
/// target dimensions.  Post-process stages finalize the result.
///
/// Usage:
/// @code
///     MipmapPipeline pipe;
///     pipe.preProcess  = { linearize };
///     pipe.downsample  = lanczos3Filter;
///     pipe.postProcess = { delinearize };
///
///     MipImage mip1 = pipe.execute(mip0, mip0.width / 2, mip0.height / 2);
/// @endcode
struct MipmapPipeline {
    std::vector<Stage> preProcess; ///< Stages applied to a **copy** of the source before filtering.
    Filter downsample;             ///< The core downsample filter.
    std::vector<Stage> postProcess; ///< Stages applied to the filtered result.

    /// Run the full pipeline on @p src, downsampling to the given target size.
    MipImage execute(const MipImage& src, u32 targetWidth, u32 targetHeight) const {
        // Pre-process operates on a copy so the caller's image is unchanged.
        MipImage input = src;
        for (const auto& stage : preProcess)
            stage(input);

        MipImage result = downsample(input, targetWidth, targetHeight);

        for (const auto& stage : postProcess)
            stage(result);

        return result;
    }
};

} // namespace whiteout::textures::mipmap
