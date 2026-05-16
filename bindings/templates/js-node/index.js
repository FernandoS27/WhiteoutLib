// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Node.js-only facade over the pthread+NODERAWFS WASM build.
//
// This wraps `whiteout-node.js` (Emscripten module, EXPORT_NAME=WhiteoutNode)
// the same way `whiteout-wasm/index.js` wraps `whiteout.js`, but adds:
//
//   - OsFileSystem  — read/write the host disk directly.
//   - SimpleThreadPool — pool that parallelises mipmap gen, BC7 encode,
//                       CASC/MPQ decode. Backed by Emscripten pthreads
//                       (= worker_threads under Node).
//   - HttpHandler subclass helper — JS implementation surfaced as the
//                       C++ HttpHandler interface for online CASC.
//   - Pool-aware texture / archive helpers.
//
// The facade is intentionally close to whiteout-wasm so user code that
// only needs models / textures works against either build with at most
// an import-path swap.

import WhiteoutNodeModule from "./whiteout-node.js";

let _modulePromise = null;
function instantiate() {
    if (_modulePromise === null) _modulePromise = WhiteoutNodeModule();
    return _modulePromise;
}

// std::vector<u8> -> Uint8Array (with copy).
function vecToBytes(vec) {
    const len = vec.size();
    const out = new Uint8Array(len);
    for (let i = 0; i < len; i++) out[i] = vec.get(i);
    vec.delete();
    return out;
}

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

function patchVectorIteration(M) {
    for (const key of Object.keys(M)) {
        if (!key.startsWith("Vector")) continue;
        const cls = M[key];
        const proto = cls?.prototype;
        if (!proto || typeof proto.size !== "function" || typeof proto.get !== "function") continue;
        if (proto[Symbol.iterator]) continue;
        proto[Symbol.iterator] = function* () {
            const n = this.size();
            for (let i = 0; i < n; i++) yield this.get(i);
        };
    }
}

function populateFromPrefix(M, prefix, target) {
    for (const key of Object.keys(M)) {
        if (key.length > prefix.length && key.startsWith(prefix)) {
            const stripped = key.slice(prefix.length);
            if (!(stripped in target)) target[stripped] = M[key];
        }
    }
    return target;
}

function makeTextureFormat(M, prefix) {
    const ParserCtor = M[prefix + "Parser"];
    const WriterCtor = M[prefix + "Writer"];
    return {
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
}

// Mirror of the web build's `whiteout.texture` namespace, with the
// pool-aware codepath wired in. Both builds export the same surface so
// downstream code that doesn't care about threading is portable.
function makeTextureNamespace(M) {
    const T = M.Texture;
    const supportsPool = typeof M.textureGenerateMipmapsWithPool === "function";

    return {
        Texture: T,
        PixelFormat: M.PixelFormat,
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

        /** Generate the full mip chain in place. Throws on failure.
         *  Pass `{ pool }` for multi-threaded BC7 / Kaiser. */
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

        downscale(tex, levels = 1) {
            const err = call(M, () => tex.downscale(levels >>> 0));
            if (err) throw new Error(`downscale failed: ${err}`);
        },

        mergeChannels(sources, channels) {
            return call(M, () => T.mergeChannels(sources, channels));
        },
    };
}

// ── HttpHandler subclass helper ─────────────────────────────────────────
// User passes a plain JS object with `getAsync(url, complete)` and optional
// `getRangeAsync(url, start, end, complete)` / `capabilities()`. We wrap
// it so the C++ wire-up sees a fully-fledged HttpHandlerWrapper subclass.
//
//   const handler = whiteout.makeHttpHandler({
//       async getAsync(url, complete) {
//           const r = await fetch(url);
//           const buf = new Uint8Array(await r.arrayBuffer());
//           complete({ statusCode: r.status, body: buf });
//       },
//   });
//
// `complete(response)` accepts `{ statusCode, body?, error? }`. Calling it
// transfers the C++ callback handle back into native land and releases
// the heap allocation.
function makeHttpHandler(M, impl) {
    const dispatch = (cbHandle, response) => {
        const { statusCode = 0, body = null, error = "" } = response || {};
        M.dispatchHttpCallback(cbHandle, statusCode | 0, body, error);
    };
    const obj = {
        capabilities() {
            return (impl.capabilities?.() ?? 0) >>> 0;
        },
        getAsync(url, cbHandle) {
            try {
                const completion = (r) => dispatch(cbHandle, r);
                const p = impl.getAsync(url, completion);
                if (p && typeof p.then === "function") {
                    p.catch((err) => dispatch(cbHandle, {
                        statusCode: 0, error: String(err?.message ?? err),
                    }));
                }
            } catch (e) {
                dispatch(cbHandle, { statusCode: 0, error: String(e?.message ?? e) });
            }
        },
        getRangeAsync(url, start, end, cbHandle) {
            try {
                const completion = (r) => dispatch(cbHandle, r);
                const fn = impl.getRangeAsync
                    ?? ((u, s, e, c) => impl.getAsync(u, c));
                const p = fn(url, start, end, completion);
                if (p && typeof p.then === "function") {
                    p.catch((err) => dispatch(cbHandle, {
                        statusCode: 0, error: String(err?.message ?? err),
                    }));
                }
            } catch (e) {
                dispatch(cbHandle, { statusCode: 0, error: String(e?.message ?? e) });
            }
        },
    };
    return M.HttpHandler.implement(obj);
}

export async function Whiteout() {
    const M = await instantiate();
    patchVectorIteration(M);

    // ── Per-format model namespaces ───────────────────────────────────────
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
        // Primary Node entry point: parse directly from disk.
        // `rootPath` is the directory containing the .m2 and its siblings;
        // `mainPath` is the .m2 file path relative to that root. The M2
        // parser pulls .skin / .skel / .anim / .bone via OsFileSystem.
        parse(rootPath, mainPath, mode = M.M2ParseMode.Lenient) {
            return call(M, () => {
                const fs = new M.OsFileSystem(rootPath);
                const p = new M.M2Parser(mode);
                try {
                    return p.parse(fs, mainPath);
                } finally { p.delete(); fs.delete(); }
            });
        },
        // Same as `parse` — kept under the more explicit name for users
        // who want to disambiguate from the web build's bytes-map form.
        parseFromDisk(rootPath, mainPath, mode = M.M2ParseMode.Lenient) {
            return this.parse(rootPath, mainPath, mode);
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

    const mpq = {
        // Node-only convenience: open a .mpq on disk, optionally with a
        // pool for parallel decompression.
        open(path, pool = null) {
            return call(M, () => M.mpqOpenWithPool(path, pool));
        },
        // Codegen only binds the no-arg Storage::save(); this surfaces
        // the save-to-path overload via a free function.
        saveTo(storage, path) {
            return call(M, () => M.mpqSaveStorageToPath(storage, path));
        },
    };
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

    // CASC namespace only present if the WASM was built with CASC support.
    const casc = M.CascStorage ? {
        Storage: M.CascStorage,
        open(path, pool = null) {
            return call(M, () => M.CascStorage.open(path, pool));
        },
    } : null;

    return {
        module: M,

        flags(...vals) {
            let n = 0;
            for (const v of vals) {
                if (v == null) continue;
                n |= (typeof v === "object" && "value" in v) ? v.value : v;
            }
            return n;
        },

        // ── Shared types at root ───────────────────────────────────────────
        Texture: M.Texture,
        PixelFormat: M.PixelFormat,
        TextureType: M.TextureType,
        Vector2f: M.Vector2f,
        Vector3f: M.Vector3f,
        Vector4f: M.Vector4f,
        Quaternion: M.Quaternion,

        // ── Node additions ────────────────────────────────────────────────
        OsFileSystem: M.OsFileSystem,
        SimpleThreadPool: M.SimpleThreadPool,

        /**
         * Create a SimpleThreadPool. Mirrors the C++ constructor:
         *   new whiteout.SimpleThreadPool(n).
         *
         * The hosting WASM binary was linked with a fixed number of
         * pre-spawned worker_threads (see -sPTHREAD_POOL_SIZE in the
         * CMake target). Requesting more than that count is legal but
         * blocks until additional pthreads are created.
         */
        threadPool(n) { return new M.SimpleThreadPool(n); },

        /**
         * Wrap a plain JS object as an HttpHandler the C++ side can call.
         * The object must expose `getAsync(url, complete)` and may expose
         * `getRangeAsync(url, start, end, complete)` and `capabilities()`.
         * `complete({ statusCode, body, error })` must be invoked exactly
         * once per request.
         */
        makeHttpHandler(impl) { return makeHttpHandler(M, impl); },

        // ── Math-type factories ───────────────────────────────────────────
        vec2: (x, y)       => ({ x, y }),
        vec3: (x, y, z)    => ({ x, y, z }),
        vec4: (x, y, z, w) => ({ x, y, z, w }),
        quat: (x, y, z, w) => ({ x, y, z, w }),

        // ── Texture pipeline helpers (factories + mip ops; pool-aware) ────
        texture: makeTextureNamespace(M),

        // ── Texture format facades ────────────────────────────────────────
        blp:  makeTextureFormat(M, "Blp"),
        dds:  makeTextureFormat(M, "Dds"),
        png:  makeTextureFormat(M, "Png"),
        jpeg: makeTextureFormat(M, "Jpeg"),
        bmp:  makeTextureFormat(M, "Bmp"),
        tga:  makeTextureFormat(M, "Tga"),

        // ── Model format namespaces ───────────────────────────────────────
        mdx, m2, m3, wem,

        // ── Storage backends ──────────────────────────────────────────────
        mpq,
        casc,
    };
}

export default Whiteout;
