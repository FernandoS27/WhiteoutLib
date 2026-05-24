// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Ergonomic JS facade over the Emscripten/Embind module.
//
// API shape mirrors the Python bindings:
//
//     const wo = await Whiteout();
//     const bone = new whiteout.mdx.Bone();           // types live under format namespaces
//     bone.node.parentId = whiteout.mdx.NoParent;     // constants too
//     model.bones.push_back(bone);
//
//     const model = whiteout.mdx.parse(bytes);        // convenience parser
//     const bytes = whiteout.mdx.write(model);
//
//     const tex = whiteout.png.parse(pngBytes);       // texture format facades
//     const blp = whiteout.blp.write(tex);
//
// `whiteout.module` is still exposed for anything not surfaced by the facade.

import WhiteoutModule from "./whiteout.js";

let _modulePromise = null;
function instantiate() {
    if (_modulePromise === null) _modulePromise = WhiteoutModule();
    return _modulePromise;
}

// Combine Embind enum values (or raw numbers) into a single bitmask.
//   whiteout.flags(whiteout.mdx.NodeFlag.Bone, whiteout.mdx.NodeFlag.Billboarded)
function flagsOf(...vals) {
    let n = 0;
    for (const v of vals) {
        if (v == null) continue;
        n |= (typeof v === "object" && "value" in v) ? v.value : v;
    }
    return n;
}

// std::vector<u8> -> Uint8Array (with copy; vector lives in WASM heap).
function vecToBytes(vec) {
    const len = vec.size();
    const out = new Uint8Array(len);
    for (let i = 0; i < len; i++) out[i] = vec.get(i);
    vec.delete();
    return out;
}

// Re-throw WASM exceptions as regular Errors so callers see useful messages.
function rethrow(M, e) {
    if (typeof WebAssembly !== "undefined" && e instanceof WebAssembly.Exception
            && typeof M.getExceptionMessage === "function") {
        const [type, msg] = M.getExceptionMessage(e);
        const out = new Error(msg || type || "WASM exception");
        out.cause = e;
        throw out;
    }
    throw e;
}

function call(M, fn) {
    try { return fn(); } catch (e) { rethrow(M, e); }
}

// Patch `[Symbol.iterator]` onto every Embind-bound vector class so callers
// can write `for (const x of vec) ...`. Embind's own surface only exposes
// indexed access (`size`/`get`); this generator yields copies, matching
// Embind's read-by-copy semantics — mutation through the iterator value
// does NOT write back to the C++ vector.
function patchVectorIteration(M) {
    for (const key of Object.keys(M)) {
        if (!key.startsWith("Vector")) continue;
        const cls = M[key];
        const proto = cls?.prototype;
        if (!proto || typeof proto.size !== "function" || typeof proto.get !== "function") {
            continue;
        }
        if (proto[Symbol.iterator]) continue;   // already patched / native
        proto[Symbol.iterator] = function* () {
            const n = this.size();
            for (let i = 0; i < n; i++) yield this.get(i);
        };
    }
}

// Auto-populate `target` with every Embind export whose name starts with
// `prefix`, with the prefix stripped. Used to build the per-format
// namespaces (`whiteout.mdx`, `whiteout.m2`, `whiteout.m3`) from the auto-generated bindings.
function populateFromPrefix(M, prefix, target) {
    for (const key of Object.keys(M)) {
        if (key.length > prefix.length && key.startsWith(prefix)) {
            const stripped = key.slice(prefix.length);
            // Don't clobber explicit method/property names already on target.
            if (!(stripped in target)) {
                target[stripped] = M[key];
            }
        }
    }
    return target;
}

function makeTextureFormat(M, prefix) {
    const ParserCtor = M[prefix + "Parser"];
    const WriterCtor = M[prefix + "Writer"];
    const ns = {
        Parser: ParserCtor,
        Writer: WriterCtor,
        ParseMode: M[prefix + "ParseMode"],
        WriteMode: M[prefix + "WriteMode"],
        parse(bytes, mode = M[prefix + "ParseMode"]?.Lenient) {
            return call(M, () => {
                const p = mode != null ? new ParserCtor(mode) : new ParserCtor();
                try { return p.parse(bytes); } finally { p.delete(); }
            });
        },
        write(texture) {
            return call(M, () => {
                const w = new WriterCtor();
                try { return vecToBytes(w.write(texture)); } finally { w.delete(); }
            });
        },
    };
    return ns;
}

// Build the `whiteout.texture` namespace — sugar over Texture's static
// factories plus pipeline helpers that turn "empty string = ok / non-empty
// = error message" into a throwing API.
//
// `opts.pool` is honoured by the Node build (whiteout-node) which exposes
// `M.textureGenerateMipmapsWithPool`/`WithCountAndPool`; in the web build
// the pool is ignored and we fall back to Texture's single-threaded
// instance method.
function makeTextureNamespace(M) {
    const T = M.Texture;
    const supportsPool = typeof M.textureGenerateMipmapsWithPool === "function";

    const ns = {
        /** Raw Embind constructor — same as `whiteout.Texture`. */
        Texture: T,
        /** PixelFormat enum (mirrors `whiteout.PixelFormat`). */
        PixelFormat: M.PixelFormat,
        /** TextureType enum (mirrors `whiteout.TextureType`). */
        TextureType: M.TextureType,

        create2D(format, width, height, mipCount = 0) {
            return call(M, () => T.create2D(format,
                width >>> 0, height >>> 0, mipCount >>> 0));
        },
        create3D(format, width, height, depth, mipCount = 0) {
            return call(M, () => T.create3D(format,
                width >>> 0, height >>> 0, depth >>> 0, mipCount >>> 0));
        },
        createCube(format, size, mipCount = 0) {
            return call(M, () => T.createCube(format,
                size >>> 0, mipCount >>> 0));
        },
        create2DArray(format, width, height, arraySize, mipCount = 0) {
            return call(M, () => T.create2DArray(format,
                width >>> 0, height >>> 0, arraySize >>> 0, mipCount >>> 0));
        },
        createCubeArray(format, size, arraySize, mipCount = 0) {
            return call(M, () => T.createCubeArray(format,
                size >>> 0, arraySize >>> 0, mipCount >>> 0));
        },

        /** Generate the full mip chain in place.
         *  Throws on failure (the C++ "" / error-message convention is
         *  hidden). Pass `{ pool }` in the Node build for multi-threaded
         *  Kaiser / BC7 — ignored everywhere else. Pass `{ newMipCount }`
         *  to override the default (preserve the existing count). */
        generateMipmaps(tex, opts = {}) {
            const { newMipCount = 0, pool = null } = opts;
            const err = call(M, () => {
                if (pool && supportsPool) {
                    return newMipCount > 0
                        ? M.textureGenerateMipmapsWithCountAndPool(
                            tex, newMipCount >>> 0, pool)
                        : M.textureGenerateMipmapsWithPool(tex, pool);
                }
                return tex.generateMipmaps(newMipCount >>> 0);
            });
            if (err) throw new Error(`generateMipmaps failed: ${err}`);
        },

        /** Drop `levels` leading mip levels in place (default 1).
         *  Throws on failure. */
        downscale(tex, levels = 1) {
            const err = call(M, () => tex.downscale(levels >>> 0));
            if (err) throw new Error(`downscale failed: ${err}`);
        },

        /** Combine N single-channel textures into one RGBA texture.
         *  `sources` is a `VectorTexture` and `channels` a `VectorChannel`
         *  (use `whiteout.module.VectorTexture` / `.VectorChannel` to
         *  build them). Returns `null` on failure. */
        mergeChannels(sources, channels) {
            return call(M, () => T.mergeChannels(sources, channels));
        },
    };
    return ns;
}

export async function Whiteout() {
    const M = await instantiate();
    patchVectorIteration(M);

    // ── Per-format model namespaces ───────────────────────────────────────
    // Each starts with hand-written convenience methods (parse/write etc.),
    // then auto-populates with every type whose JS name carries the prefix.

    const mdx = {
        /** Parse binary MDX bytes into a Model.
         *  @param bytes   Uint8Array of MDX file contents.
         *  @param upgrade Optional UpgradeMode (default: UpgradeOldVersions). */
        parse(bytes, upgrade = M.MdxUpgradeMode.UpgradeOldVersions) {
            return call(M, () => {
                const p = new M.MdxParser(upgrade);
                try { return p.parse_buffer_format(bytes, M.MdxMDLXFormat.MDX); }
                finally { p.delete(); }
            });
        },
        /** Parse text MDL bytes into a Model.
         *  @param bytes   Uint8Array of MDL file contents (UTF-8 text).
         *  @param upgrade Optional UpgradeMode (default: UpgradeOldVersions). */
        parseMdl(bytes, upgrade = M.MdxUpgradeMode.UpgradeOldVersions) {
            return call(M, () => {
                const p = new M.MdxParser(upgrade);
                try { return p.parse_buffer_format(bytes, M.MdxMDLXFormat.MDL); }
                finally { p.delete(); }
            });
        },
        /** Encode a Model as binary MDX bytes. */
        write(model) {
            return call(M, () => {
                const w = new M.MdxWriter();
                try {
                    return vecToBytes(w.write_mdx_format_mdlFormat(
                        model, M.MdxMDLXFormat.MDX, M.MdxMdlFormat.WarcraftIII));
                } finally { w.delete(); }
            });
        },
        /** Encode a Model as text MDL bytes.
         *  @param model   The MDX Model to serialise.
         *  @param dialect Optional MDL dialect (default: WarcraftIII engine-faithful;
         *                 pass `whiteout.mdx.MdlFormat.Hiveworkshop` for the
         *                 community-tool dialect). */
        writeMdl(model, dialect = M.MdxMdlFormat.WarcraftIII) {
            return call(M, () => {
                const w = new M.MdxWriter();
                try {
                    return vecToBytes(w.write_mdx_format_mdlFormat(
                        model, M.MdxMDLXFormat.MDL, dialect));
                } finally { w.delete(); }
            });
        },
    };
    populateFromPrefix(M, "Mdx", mdx);

    const m2 = {
        // files: Record<string, Uint8Array>
        // mainPath: path key in `files` of the base .m2
        parse(files, mainPath, mode = M.M2ParseMode.Lenient) {
            return call(M, () => {
                const fs = new M.InMemoryFileSystem();
                const p = new M.M2Parser(mode);
                try {
                    for (const [path, data] of Object.entries(files)) {
                        fs.addFile(path, data);
                    }
                    return p.parse(fs, mainPath);
                } finally { p.delete(); fs.delete(); }
            });
        },
    };
    populateFromPrefix(M, "M2", m2);

    const m3 = {
        parse(bytes, mode = M.M3ParseMode.Lenient) {
            return call(M, () => {
                const p = new M.M3Parser(mode);
                try { return p.parse(bytes); } finally { p.delete(); }
            });
        },
        write(model) {
            return call(M, () => {
                const w = new M.M3Writer();
                try { return vecToBytes(w.write(model)); } finally { w.delete(); }
            });
        },
    };
    populateFromPrefix(M, "M3", m3);

    // MPQ archive surface — type registry only; users go through
    // `whiteout.mpq.Storage.open(path)` / `.create(opts)`.
    const mpq = {};
    populateFromPrefix(M, "Mpq", mpq);

    const wem = {
        parse(bytes, mode = M.WemParseMode.Lenient) {
            return call(M, () => {
                const p = new M.WemParser(mode);
                try { return p.parse(bytes); } finally { p.delete(); }
            });
        },
        write(model) {
            return call(M, () => {
                const w = new M.WemWriter();
                try { return vecToBytes(w.write(model)); } finally { w.delete(); }
            });
        },
    };
    populateFromPrefix(M, "Wem", wem);

    return {
        // Raw module — escape hatch for anything not surfaced by the facade.
        module: M,

        /** Combine Embind enum values (or raw integers) into a bitmask. */
        flags: flagsOf,

        // ── Shared types at root ───────────────────────────────────────────
        Texture: M.Texture,
        PixelFormat: M.PixelFormat,
        TextureType: M.TextureType,
        Vector2f: M.Vector2f,
        Vector3f: M.Vector3f,
        Vector4f: M.Vector4f,
        Quaternion: M.Quaternion,
        InMemoryFileSystem: M.InMemoryFileSystem,

        // ── Math-type factories ────────────────────────────────────────────
        // Vector2f/3f/4f and Quaternion are Embind value_object types —
        // they pass as plain JS literals. These factories are syntactic
        // sugar so callers don't have to spell the field names.
        vec2: (x, y)       => ({ x, y }),
        vec3: (x, y, z)    => ({ x, y, z }),
        vec4: (x, y, z, w) => ({ x, y, z, w }),
        quat: (x, y, z, w) => ({ x, y, z, w }),

        // ── Texture pipeline helpers (factories + mip ops) ────────────────
        texture: makeTextureNamespace(M),

        // ── Texture format facades (parse/write helpers + raw classes) ────
        blp:  makeTextureFormat(M, "Blp"),
        dds:  makeTextureFormat(M, "Dds"),
        png:  makeTextureFormat(M, "Png"),
        jpeg: makeTextureFormat(M, "Jpeg"),
        bmp:  makeTextureFormat(M, "Bmp"),
        tga:  makeTextureFormat(M, "Tga"),

        // ── Model format namespaces (types + parse/write) ─────────────────
        mdx, m2, m3, wem,

        // ── Storage backends ──────────────────────────────────────────────
        mpq,
    };
}

export default Whiteout;
