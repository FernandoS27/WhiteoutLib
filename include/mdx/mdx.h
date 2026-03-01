#pragma once

/**
 * @file mdx.h
 * @brief Main header for the MDX (Warcraft III Model Format) library
 * 
 * This is the primary include file for the MDX library. Include this single header
 * to access all MDX functionality including parsing, writing, and structure definitions.
 * 
 * The MDX format is used by Warcraft III and Warcraft III: Reforged for 3D models.
 * This library supports versions 800 (Classic) through 1200 (Reforged).
 * 
 * @example Basic Usage
 * @code
 * #include <mdx/mdx.h>
 * 
 * // Parse an MDX file
 * mdx::MDXParser parser;
 * mdx::MDXFile model = parser.parse("model.mdx");
 * 
 * // Modify the model...
 * model.modelName = "Modified Model";
 * 
 * // Write it back
 * mdx::MDXWriter writer;
 * writer.write("output.mdx", model);
 * @endcode
 */

#include "types.h"
#include "structures.h"
#include "mdx_parser.h"
#include "mdx_writer.h"

namespace whiteout {
namespace mdx {
    /// Library major version number
    constexpr int MAJOR_VERSION = 1;
    /// Library minor version number
    constexpr int MINOR_VERSION = 0;
    /// Library patch version number
    constexpr int PATCH_VERSION = 0;
} // namespace mdx
} // namespace whiteout
