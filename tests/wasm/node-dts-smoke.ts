// SPDX-License-Identifier: BSD-3-Clause
// TypeScript usage smoke test for whiteout-node — type-check only.
//
// Verifies the Node-flavoured `.d.ts` shape matches how callers actually
// use the library, including the OsFileSystem / SimpleThreadPool /
// HttpHandler additions and the unified `whiteout.texture` namespace.

import type {
    WhiteoutNodeAPI, MdxNamespace, M2Namespace,
    OsFileSystem, SimpleThreadPool, HttpHandler, HttpHandlerImpl, HttpResponse,
    Vector3f, Quaternion, Texture, EnumValue,
} from "../../packages/js-node/index";
import type * as mdx from "../../packages/js-node/types/mdx";
import type * as textures from "../../packages/js-node/types/textures";

declare const whiteout: WhiteoutNodeAPI;


// ── 1. Same MDX/M2/M3 surface as the web build ────────────────────────

const bone: mdx.Bone = new whiteout.mdx.Bone();
bone.node.parentId = whiteout.mdx.NoParent;
bone.geosetId      = whiteout.mdx.MultipleGeosets;

const model = new whiteout.mdx.Model();
model.bones.push_back(bone);


// ── 2. Node-only OsFileSystem + M2 disk parse ─────────────────────────

const fs: OsFileSystem = new whiteout.OsFileSystem("C:/Games/Wow/Data");
const exists: boolean = fs.fileExists("character/human/character.m2");
fs.delete();

// `m2.parse` takes rootPath + mainPath (no bytes map).
const m2model = whiteout.m2.parse(
    "C:/Games/Wow/Data/character/human",
    "character.m2",
);
m2model.delete();


// ── 3. SimpleThreadPool + texture pipeline ────────────────────────────

const pool: SimpleThreadPool = whiteout.threadPool(8);
const threads: number = pool.threadCount();

declare const pngBytes: Uint8Array;
const tex: Texture = whiteout.png.parse(pngBytes);

// New unified namespace — same shape as the web build, but the pool
// arg actually does something here.
whiteout.texture.generateMipmaps(tex, { pool });
whiteout.texture.downscale(tex, 2);

const blank: Texture = whiteout.texture.create2D(
    whiteout.PixelFormat.RGBA8, 256, 256,
);
blank.delete();
tex.delete();
pool.delete();


// ── 4. HttpHandler trampoline ─────────────────────────────────────────

const impl: HttpHandlerImpl = {
    async getAsync(url, complete) {
        const r = await fetch(url);
        complete({
            statusCode: r.status,
            body: new Uint8Array(await r.arrayBuffer()),
        } satisfies HttpResponse);
    },
};
const handler: HttpHandler = whiteout.makeHttpHandler(impl);
const caps: number = handler.capabilities();
handler.delete();


// ── 5. MPQ on-disk surface ────────────────────────────────────────────

const archive = whiteout.mpq.open("War3.mpq");
if (archive) {
    const bytes: Uint8Array | null = archive.readFile("war3map.j");
    archive.delete();
}


// All references above MUST type-check. If `tsc` fails, the Node public
// surface has drifted from the codegen output (or from the hand-written
// _shared.d.ts).
export {};
