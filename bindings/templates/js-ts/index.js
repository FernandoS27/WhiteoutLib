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

export async function Whiteout() {
    const M = await instantiate();
    patchVectorIteration(M);

    // ── Per-format model namespaces ───────────────────────────────────────
    // Each starts with hand-written convenience methods (parse/write etc.),
    // then auto-populates with every type whose JS name carries the prefix.

    const mdx = {
        parse(bytes, mode = M.MdxParseMode.Lenient,
              upgrade = M.MdxUpgradeMode.UpgradeOldVersions) {
            return call(M, () => {
                const p = new M.MdxParser(mode, upgrade);
                try { return p.parseMdx(bytes); } finally { p.delete(); }
            });
        },
        parseMdl(bytes, mode = M.MdxParseMode.Lenient,
                 upgrade = M.MdxUpgradeMode.UpgradeOldVersions) {
            return call(M, () => {
                const p = new M.MdxParser(mode, upgrade);
                try { return p.parseMdl(bytes); } finally { p.delete(); }
            });
        },
        write(model) {
            return call(M, () => {
                const w = new M.MdxWriter();
                try { return vecToBytes(w.writeMdx(model)); } finally { w.delete(); }
            });
        },
        writeMdl(model) {
            return call(M, () => {
                const w = new M.MdxWriter();
                try { return vecToBytes(w.writeMdl(model)); } finally { w.delete(); }
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
