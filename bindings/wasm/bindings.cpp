// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Embind bindings exposing whiteout_lib's models and textures to JavaScript.
//
// Design choices:
//   - Inputs accept JS Uint8Array; we convert to std::vector<u8> on entry.
//   - Outputs return std::vector<u8>; the JS facade wraps as Uint8Array.
//   - Models are exposed as opaque handles with a small number of count
//     accessors. Full structure introspection is out of scope for the WASM
//     port; round-trip (parse -> write -> re-parse) is the primary use case.
//   - All optional WorkerPool* parameters are passed nullptr (single-threaded).

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/blp/parser.h>
#include <whiteout/textures/blp/writer.h>
#include <whiteout/textures/dds/parser.h>
#include <whiteout/textures/dds/writer.h>
#include <whiteout/textures/png/parser.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/jpeg/parser.h>
#include <whiteout/textures/jpeg/writer.h>
#include <whiteout/textures/bmp/parser.h>
#include <whiteout/textures/bmp/writer.h>
#include <whiteout/textures/tga/parser.h>
#include <whiteout/textures/tga/writer.h>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/mdx/writer.h>
#include <whiteout/models/m3/parser.h>
#include <whiteout/models/m3/writer.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/writer.h>
#include <whiteout/models/m2/parser.h>

// InMemoryFileSystem is the web build's only VirtualPathFileSystem
// implementation. The Node build links its own OsFileSystem instead and
// doesn't need (or compile) the in-memory variant — guarded below.
#ifndef WHITEOUT_WASM_NODE_BUILD
#include "in_memory_fs.h"
#endif

using namespace emscripten;
using namespace whiteout;

namespace {

// Convert a JS Uint8Array (or any number-typed array) into a std::vector<u8>.
std::vector<u8> jsToBytes(const val& jsArray) {
    return convertJSArrayToNumberVector<u8>(jsArray);
}

// Throw a JS-visible runtime_error with a helpful message.
[[noreturn]] void fail(const std::string& msg) {
    throw std::runtime_error(msg);
}

// ── Model parser/writer glue ─────────────────────────────────────────────

mdx::Model parseMdx(mdx::Parser& parser, const val& jsArray) {
    const auto bytes = jsToBytes(jsArray);
    return parser.parse(std::span<const u8>(bytes.data(), bytes.size()),
                        mdx::MDLXFormat::MDX);
}

mdx::Model parseMdl(mdx::Parser& parser, const val& jsArray) {
    const auto bytes = jsToBytes(jsArray);
    return parser.parse(std::span<const u8>(bytes.data(), bytes.size()),
                        mdx::MDLXFormat::MDL);
}

std::vector<u8> writeMdx(mdx::Writer& writer, const mdx::Model& model) {
    return writer.write(model, mdx::MDLXFormat::MDX, mdx::MdlFormat::WarcraftIII);
}

std::vector<u8> writeMdl(mdx::Writer& writer, const mdx::Model& model) {
    return writer.write(model, mdx::MDLXFormat::MDL, mdx::MdlFormat::WarcraftIII);
}

m3::Model parseM3(m3::Parser& parser, const val& jsArray) {
    const auto bytes = jsToBytes(jsArray);
    return parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
}

std::vector<u8> writeM3(m3::Writer& writer, const m3::Model& model) {
    return writer.write(model);
}

models::wem::Document parseWem(models::wem::Parser& parser, const val& jsArray) {
    const auto bytes = jsToBytes(jsArray);
    auto result = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    if (!result) fail("wem parse returned no result");
    return std::move(*result);
}

// The unknown chunks a parse preserved are deliberately not threaded through
// here: JS holds a `Document`, not a parser, so there is nothing to hand back
// and a silent drop is the honest default rather than a hidden one.
std::vector<u8> writeWem(models::wem::Writer& writer, const models::wem::Document& document) {
    return writer.write(document);
}

// Accepts any concrete VirtualPathFileSystem: InMemoryFileSystem in the
// web build, OsFileSystem in the Node build. Embind handles the upcast
// because both register `.base<VirtualPathFileSystem>()`.
m2::Model parseM2(m2::Parser& parser, interfaces::VirtualPathFileSystem& fs,
                  const std::string& path) {
    return parser.parse(fs, path);
}

#ifndef WHITEOUT_WASM_NODE_BUILD
// ── In-memory FS helper (web build only) ─────────────────────────────────

void fsAddFile(wasm::InMemoryFileSystem& fs, const std::string& path,
               const val& jsArray) {
    fs.addFile(path, jsToBytes(jsArray));
}
#endif

} // namespace

EMSCRIPTEN_BINDINGS(whiteout) {
    // ── Vector containers used as I/O ────────────────────────────────────
    register_vector<u8>("VectorU8");
    register_vector<std::string>("VectorString");

    // Texture, all PixelFormat / TextureType enums, and all per-format
    // Parser/Writer classes are bound by tools/codegen via
    // bindings/wasm/textures_bindings.cpp.

    // Per-format model bindings (MdxParser / MdxWriter / M3Parser /
    // M3Writer / M2Parser / WemParser / ...) plus their enums (MdxUpgradeMode,
    // MdxMDLXFormat, MdxMdlFormat, ...) are all emitted by the codegen
    // sibling translation units: mdx_bindings.cpp, m3_bindings.cpp,
    // m2_bindings.cpp. They used to be hand-bound here, which now causes
    // "Cannot register type 'MdxUpgradeMode' twice" at module init.

    // ── Abstract VirtualPathFileSystem base class ────────────────────────
    // Registered so concrete subclasses (InMemoryFileSystem in the web
    // build, OsFileSystem in the Node build) can declare
    // `.base<VirtualPathFileSystem>()` in their bindings.
    class_<interfaces::VirtualPathFileSystem>("VirtualPathFileSystem");

#ifndef WHITEOUT_WASM_NODE_BUILD
    // ── In-memory VirtualPathFileSystem (web build only) ─────────────────
    // Used by `m2.parse({path: bytes}, mainPath)` to give the M2 parser
    // its sibling .skin / .skel / .anim / .bone files when there's no
    // real filesystem available. The Node build replaces this with
    // OsFileSystem (bound in node_bindings.cpp).
    class_<wasm::InMemoryFileSystem, base<interfaces::VirtualPathFileSystem>>(
        "InMemoryFileSystem")
        .constructor<>()
        .function("addFile", &fsAddFile)
        .function("removeFile", &wasm::InMemoryFileSystem::removeFile)
        .function("clear", &wasm::InMemoryFileSystem::clear)
        .function("fileCount",
                  optional_override([](const wasm::InMemoryFileSystem& fs) {
                      return static_cast<u32>(fs.fileCount());
                  }))
        .function("fileExists", &wasm::InMemoryFileSystem::fileExists);
#endif
}
