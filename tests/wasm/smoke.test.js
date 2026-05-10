// SPDX-License-Identifier: BSD-3-Clause
//
// Minimal smoke tests for the JS facade.
//
// API shape mirrors the Python bindings:
//
//     whiteout.mdx.Bone, whiteout.m2.Sequence, whiteout.mdx.NoParent
//     whiteout.png.parse(bytes), whiteout.blp.write(texture)
//     whiteout.Texture, whiteout.Vector3f, whiteout.PixelFormat
//     whiteout.module                — escape hatch to the raw Embind module
//
// Pre-requisite: ../../package/whiteout.js + whiteout.wasm must exist.
// Build them via `pwsh ../../scripts/build-wasm.ps1`.

import { test } from "node:test";
import { strict as assert } from "node:assert";
import { deflateSync } from "node:zlib";
import { Whiteout } from "../../package/index.js";

// Build a valid 2x2 RGB PNG dynamically (avoid hand-rolled CRCs).
function makePng(width, height, rgbPixels) {
    function crc32(buf) {
        const table = new Uint32Array(256);
        for (let n = 0; n < 256; n++) {
            let c = n;
            for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
            table[n] = c;
        }
        let crc = 0xffffffff;
        for (let i = 0; i < buf.length; i++) crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8);
        return (crc ^ 0xffffffff) >>> 0;
    }
    function chunk(type, data) {
        const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
        const typed = Buffer.from(type, "ascii");
        const crc = Buffer.alloc(4);
        crc.writeUInt32BE(crc32(Buffer.concat([typed, data])), 0);
        return Buffer.concat([len, typed, data, crc]);
    }
    const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(width, 0);
    ihdr.writeUInt32BE(height, 4);
    ihdr[8] = 8;
    ihdr[9] = 2;
    const stride = width * 3;
    const raw = Buffer.alloc(height * (stride + 1));
    for (let y = 0; y < height; y++) {
        raw[y * (stride + 1)] = 0;
        for (let x = 0; x < stride; x++) {
            raw[y * (stride + 1) + 1 + x] = rgbPixels[y * stride + x];
        }
    }
    return new Uint8Array(Buffer.concat([
        sig, chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)),
        chunk("IEND", Buffer.alloc(0)),
    ]));
}

const ONE_PIXEL_RED_PNG = makePng(1, 1, [255, 0, 0]);


// ── Helpers ─────────────────────────────────────────────────────────────
// Vector / string properties on Embind class objects return COPIES; to
// mutate in place use read-modify-write. Math types (Vector3f, Quaternion,
// Extent) are value_object — pass plain JS objects, no .delete() needed.
function withField(obj, name, mutate) {
    const v = obj[name];
    try { mutate(v); obj[name] = v; } finally { v.delete(); }
}


// ── Texture surface ─────────────────────────────────────────────────────

test("png round-trip", async () => {
    const whiteout = await Whiteout();

    const tex = whiteout.png.parse(ONE_PIXEL_RED_PNG);
    assert.equal(tex.width(), 1);
    assert.equal(tex.height(), 1);

    const reEncoded = whiteout.png.write(tex);
    assert.ok(reEncoded.length >= 33);
    assert.deepEqual(
        reEncoded.slice(0, 8),
        new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    );

    const reTex = whiteout.png.parse(reEncoded);
    assert.equal(reTex.width(), 1);

    tex.delete();
    reTex.delete();
});

test("blp parser rejects garbage", async () => {
    const whiteout = await Whiteout();
    const garbage = new Uint8Array([0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe]);
    assert.throws(() => whiteout.blp.parse(garbage));
});


// ── MDX ─────────────────────────────────────────────────────────────────

test("mdx parser is lenient on garbage", async () => {
    const whiteout = await Whiteout();
    const garbage = new Uint8Array([0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef]);
    let model;
    try {
        model = whiteout.mdx.parse(garbage);
        assert.equal(model.geosets.size(), 0);
        assert.equal(model.bones.size(), 0);
    } finally {
        model?.delete();
    }
});

test("mdx round-trip empty model", async () => {
    const whiteout = await Whiteout();
    const empty = new whiteout.mdx.Model();
    try {
        const bytes = whiteout.mdx.write(empty);
        assert.ok(bytes.length > 0);

        const reParsed = whiteout.mdx.parse(bytes);
        try {
            assert.equal(reParsed.geosets.size(), 0);
        } finally {
            reParsed.delete();
        }
    } finally {
        empty.delete();
    }
});

test("mdx full type tree: build, mutate, round-trip", async () => {
    const whiteout = await Whiteout();

    const model = new whiteout.mdx.Model();
    try {
        model.version = 800;
        model.modelName = "TestModel";
        model.blendTime = 150;

        // Extent / Vector3f are value_object — plain JS object literals.
        model.modelExtent = {
            boundsRadius: 42.0,
            minimum: { x: -1, y: -2, z: -3 },
            maximum: { x:  4, y:  5, z:  6 },
        };

        withField(model, "sequences", (seqs) => {
            const seq = new whiteout.mdx.Sequence();
            seq.name = "Stand";
            seq.intervalEnd = 33;
            seq.flags = whiteout.mdx.SequenceFlag.None;
            seq.rarity = 1.5;
            seqs.push_back(seq);
            seq.delete();
        });

        withField(model, "textures", (texs) => {
            const tex = new whiteout.mdx.Texture();
            tex.replaceableId = 1;
            tex.fileName = "Textures/test.blp";
            tex.flags = whiteout.mdx.TextureFlag.WrapWidth;
            texs.push_back(tex);
            tex.delete();
        });

        withField(model, "materials", (mats) => {
            const mat = new whiteout.mdx.Material();
            mat.priorityPlane = 5;
            mat.flags = whiteout.mdx.MaterialFlag.TwoSided;
            withField(mat, "layers", (layers) => {
                const layer = new whiteout.mdx.Layer();
                layer.filterMode = whiteout.mdx.LayerFilterMode.Blend;
                layer.alpha = 0.75;
                layers.push_back(layer);
                layer.delete();
            });
            mats.push_back(mat);
            mat.delete();
        });

        withField(model, "bones", (bones) => {
            const bone = new whiteout.mdx.Bone();
            const node = new whiteout.mdx.Node();
            node.name = "Bone_Root";
            node.parentId = whiteout.mdx.NoParent;
            node.flags = whiteout.mdx.NodeFlag.Bone;
            node.type = whiteout.mdx.NodeType.Bone;
            bone.node = node;
            bone.geosetId = whiteout.mdx.MultipleGeosets;
            bone.geosetAnimationId = whiteout.mdx.MultipleGeosets;
            bones.push_back(bone);
            node.delete(); bone.delete();
        });

        withField(model, "pivotPoints", (pp) => {
            pp.push_back({ x: 0.5, y: 1.0, z: 1.5 });
        });

        const bytes = whiteout.mdx.write(model);
        assert.ok(bytes.length > 100);

        const reParsed = whiteout.mdx.parse(bytes);
        try {
            assert.equal(reParsed.modelName, "TestModel");
            assert.equal(reParsed.modelExtent.boundsRadius, 42.0);
            assert.equal(reParsed.sequences.size(), 1);
            assert.equal(reParsed.bones.size(), 1);

            const s0 = reParsed.sequences.get(0);
            assert.equal(s0.name, "Stand");
            s0.delete();

            const b0 = reParsed.bones.get(0);
            assert.equal(b0.node.name, "Bone_Root");
            b0.delete();

            const pivot = reParsed.pivotPoints.get(0);
            assert.ok(Math.abs(pivot.x - 0.5) < 1e-5);
        } finally {
            reParsed.delete();
        }
    } finally {
        model.delete();
    }
});

test("mdx exposes static sentinel constants", async () => {
    const whiteout = await Whiteout();
    assert.equal(whiteout.mdx.NoParent,         0xFFFFFFFF);
    assert.equal(whiteout.mdx.MultipleGeosets,  0xFFFFFFFF);
    assert.equal(whiteout.mdx.NoGlobalSequence, 0xFFFFFFFF);
});

test("mdx ShaderType covers full Reforged enum", async () => {
    const whiteout = await Whiteout();
    assert.equal(whiteout.mdx.LayerShaderType.DebugTexture.value, 9);
    assert.equal(whiteout.mdx.LayerShaderType.GaussianBlur.value, 13);
    assert.equal(whiteout.mdx.LayerShaderType.Imgui.value, 25);
});

test("mdx NodeFlag aliases share underlying values", async () => {
    const whiteout = await Whiteout();
    const F = whiteout.mdx.NodeFlag;
    assert.equal(F.Unshaded.value,        0x8000);
    assert.equal(F.EmitterUsesMdl.value,  0x8000);
    assert.equal(F.SortPrimitives.value,  0x10000);
    assert.equal(F.EmitterUsesTga.value,  0x10000);
    assert.equal(F.LineEmitter.value,     0x20000);
    assert.equal(F.PopcornUnfogged.value, 0x20000);
    assert.equal(F.Unfogged.value,        0x40000);
    assert.equal(F.PopcornScaling.value,  0x40000);
});

test("mdx Geoset.vertexGroups round-trip + view path", async () => {
    const whiteout = await Whiteout();
    const g = new whiteout.mdx.Geoset();
    try {
        const vg = g.vertexGroups;
        vg.push_back(7);
        vg.push_back(11);
        g.vertexGroups = vg;
        vg.delete();

        const view = g.vertexGroupsView();
        assert.equal(view.length, 2);
        assert.equal(view[0], 7);
        assert.equal(view[1], 11);
    } finally {
        g.delete();
    }
});

test("mdx Track<Quaternion> keys vector is registered", async () => {
    const whiteout = await Whiteout();
    const tr = new whiteout.mdx.TrackQuaternion();
    try {
        const keys = tr.keys;
        keys.push_back({ x: 0, y: 0, z: 0, w: 1 });
        tr.keys = keys;
        keys.delete();

        const after = tr.keys;
        assert.equal(after.size(), 1);
        assert.equal(after.get(0).w, 1);
        after.delete();
    } finally {
        tr.delete();
    }
});

test("ParticleEmitter2 segment setter throws on wrong size", async () => {
    const whiteout = await Whiteout();
    const pe = new whiteout.mdx.ParticleEmitter2();
    try {
        const v3 = new whiteout.module.VectorVector3f();
        try {
            v3.push_back({ x: 1, y: 0, z: 0 });
            v3.push_back({ x: 0, y: 1, z: 0 });
            assert.throws(() => pe.setSegmentColor(v3));
        } finally {
            v3.delete();
        }
    } finally {
        pe.delete();
    }
});

test("mdx Track<f32> field is wired through", async () => {
    const whiteout = await Whiteout();

    const model = new whiteout.mdx.Model();
    try {
        const ga = new whiteout.mdx.GeosetAnimation();
        ga.alpha = 0.5;
        ga.geosetId = 0xFFFFFFFF;

        const tr = new whiteout.mdx.TrackF32();
        tr.isUsed = true;
        tr.interpolationType = whiteout.mdx.InterpolationType.Linear;
        tr.keyCount = 2;
        withField(tr, "timestamps", (ts) => { ts.push_back(0); ts.push_back(33); });
        withField(tr, "keys", (ks) => { ks.push_back(1.0); ks.push_back(0.0); });
        ga.alphaTracks = tr;
        tr.delete();

        withField(model, "geosetAnimations", (gas) => gas.push_back(ga));
        ga.delete();

        const bytes = whiteout.mdx.write(model);
        const reParsed = whiteout.mdx.parse(bytes);
        try {
            assert.equal(reParsed.geosetAnimations.size(), 1);
            const ga2 = reParsed.geosetAnimations.get(0);
            const tracks = ga2.alphaTracks;
            assert.equal(tracks.isUsed, true);
            assert.equal(tracks.timestamps.size(), 2);
            assert.equal(tracks.keys.size(), 2);
            tracks.delete();
            ga2.delete();
        } finally {
            reParsed.delete();
        }
    } finally {
        model.delete();
    }
});


// ── .view() — zero-copy TypedArray over WASM heap ──────────────────────

test("vector .view() — primitive Float32Array, zero-copy + writable", async () => {
    const whiteout = await Whiteout();

    const tr = new whiteout.mdx.TrackF32();
    try {
        const keys = tr.keys;
        try {
            keys.push_back(1.0);
            keys.push_back(2.0);
            keys.push_back(3.0);

            const view = keys.view();
            assert.ok(view instanceof Float32Array, "expected Float32Array");
            assert.equal(view.length, 3);
            assert.deepEqual([...view], [1.0, 2.0, 3.0]);

            // Mutate through the view and verify the C++ side sees it.
            view[1] = 99.0;
            assert.equal(keys.get(1), 99.0);
        } finally {
            keys.delete();
        }
    } finally {
        tr.delete();
    }
});

test("vector .view() — primitive Uint32Array for u32 timestamps", async () => {
    const whiteout = await Whiteout();
    const tr = new whiteout.mdx.TrackF32();
    try {
        const ts = tr.timestamps;
        try {
            ts.push_back(0);
            ts.push_back(33);
            ts.push_back(66);
            const view = ts.view();
            assert.ok(view instanceof Uint32Array);
            assert.deepEqual([...view], [0, 33, 66]);
        } finally {
            ts.delete();
        }
    } finally {
        tr.delete();
    }
});

test("vector .view() — Vector3f flattens to Float32Array of length N*3", async () => {
    const whiteout = await Whiteout();

    const model = new whiteout.mdx.Model();
    try {
        const pivots = model.pivotPoints;
        try {
            pivots.push_back({ x: 1, y: 2, z: 3 });
            pivots.push_back({ x: 4, y: 5, z: 6 });

            const view = pivots.view();
            assert.ok(view instanceof Float32Array);
            assert.equal(view.length, 6);
            // Layout is [x0,y0,z0, x1,y1,z1].
            assert.deepEqual([...view], [1, 2, 3, 4, 5, 6]);

            // Stride access matches indexed get().
            const p1 = pivots.get(1);
            assert.equal(view[1*3 + 0], p1.x);
            assert.equal(view[1*3 + 1], p1.y);
            assert.equal(view[1*3 + 2], p1.z);
        } finally {
            pivots.delete();
        }
    } finally {
        model.delete();
    }
});

test("vector .view() — Quaternion exposes flat Float32Array(N*4)", async () => {
    const whiteout = await Whiteout();
    const tr = new whiteout.mdx.TrackQuaternion();
    try {
        const keys = tr.keys;
        try {
            keys.push_back({ x: 0, y: 0, z: 0, w: 1 });
            keys.push_back({ x: 1, y: 0, z: 0, w: 0 });
            const view = keys.view();
            assert.ok(view instanceof Float32Array);
            assert.equal(view.length, 8);
            assert.deepEqual([...view.slice(0, 4)], [0, 0, 0, 1]);
            assert.deepEqual([...view.slice(4, 8)], [1, 0, 0, 0]);
        } finally {
            keys.delete();
        }
    } finally {
        tr.delete();
    }
});


// ── M2 / M3 ─────────────────────────────────────────────────────────────

test("m2 model: full type tree exposed", async () => {
    const whiteout = await Whiteout();
    const m = new whiteout.m2.Model();
    try {
        m.modelName = "TestModel";
        assert.equal(m.bones.size(), 0);
        assert.equal(m.vertices.size(), 0);

        const bones = m.bones;
        const b = new whiteout.m2.Bone();
        bones.push_back(b);
        m.bones = bones;
        b.delete(); bones.delete();
        assert.equal(m.bones.size(), 1);

        m.bounding = {
            minimum: { x: -1, y: -2, z: -3 },
            maximum: { x:  1, y:  2, z:  3 },
            sphereRadius: 5.0,
        };
        assert.equal(m.bounding.sphereRadius, 5.0);
    } finally {
        m.delete();
    }
});

test("m2 enums live under whiteout.m2 namespace", async () => {
    const whiteout = await Whiteout();
    assert.ok(whiteout.m2.BoneFlag);
    assert.ok(whiteout.m2.GlobalFlag);
    assert.ok(whiteout.m2.SequenceFlag);
    assert.ok(whiteout.m2.MaterialFlag);
    assert.ok(whiteout.m2.ParticleEmitterType);
    assert.ok(whiteout.m2.ParticleBlending);
});

test("m3 model: full type tree exposed", async () => {
    const whiteout = await Whiteout();
    const m = new whiteout.m3.Model();
    try {
        m.skinBoneCount = 42;
        m.name = "M3Test";
        assert.equal(m.skinBoneCount, 42);
        assert.equal(m.name, "M3Test");

        const bones = m.bones;
        bones.push_back(new whiteout.m3.Bone());
        m.bones = bones;
        bones.delete();
        assert.equal(m.bones.size(), 1);
    } finally {
        m.delete();
    }
});

test("m3 BlendMode is rich and lives under whiteout.m3", async () => {
    const whiteout = await Whiteout();
    assert.ok(whiteout.m3.BlendMode);
    const values = Object.keys(whiteout.m3.BlendMode);
    assert.ok(values.length >= 5);
});


// ── In-memory FS ────────────────────────────────────────────────────────

test("InMemoryFileSystem accepts files and normalises slashes", async () => {
    const whiteout = await Whiteout();
    const fs = new whiteout.InMemoryFileSystem();
    try {
        fs.addFile("a/b.bin", new Uint8Array([1, 2, 3]));
        assert.equal(fs.fileCount(), 1);
        assert.equal(fs.fileExists("a/b.bin"), true);
        assert.equal(fs.fileExists("a\\b.bin"), true);
        assert.equal(fs.fileExists("missing"), false);
    } finally {
        fs.delete();
    }
});
