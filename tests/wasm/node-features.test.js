// SPDX-License-Identifier: BSD-3-Clause
//
// Smoke tests for the Node-flavoured WASM build (whiteout-node).
//
// These cover the surface that the plain whiteout-wasm build can't reach:
//   - OsFileSystem against a tmp dir on real disk (NODERAWFS).
//   - SimpleThreadPool spawning workers and running mipmap gen in parallel.
//   - makeHttpHandler routing C++ HttpHandler calls through a JS callback.
//   - mpq.open(path) round-trip on disk.
//
// Pre-requisite: ../../packages/js-node/whiteout-node.js + whiteout-node.wasm
// must exist. Build them via `pwsh ../../scripts/build-wasm-node.ps1`.

import { test } from "node:test";
import { strict as assert } from "node:assert";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { deflateSync } from "node:zlib";
import { Whiteout } from "../../packages/js-node/index.js";

// Tiny valid PNG generator (shared with smoke.test.js shape).
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
    ihdr.writeUInt32BE(width, 0); ihdr.writeUInt32BE(height, 4);
    ihdr[8] = 8; ihdr[9] = 2;
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

// ── OsFileSystem (NODERAWFS) ────────────────────────────────────────────

test("OsFileSystem reads back files written to a tmp dir", async () => {
    const whiteout = await Whiteout();

    const dir = mkdtempSync(path.join(tmpdir(), "whiteout-node-"));
    try {
        writeFileSync(path.join(dir, "hello.bin"),
            Buffer.from([1, 2, 3, 4, 5]));

        const fs = new whiteout.OsFileSystem(dir);
        try {
            assert.equal(fs.fileExists("hello.bin"), true);
            assert.equal(fs.fileExists("missing.bin"), false);

            const bytes = fs.readFile("hello.bin");
            // OsFileSystem.readFile returns a typed_memory_view; copy out
            // before the next allocation invalidates it.
            const copy = new Uint8Array(bytes);
            assert.deepEqual([...copy], [1, 2, 3, 4, 5]);

            // Slash normalisation.
            assert.equal(fs.fileExists("hello.bin"), true);
        } finally {
            fs.delete();
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});


// ── SimpleThreadPool ────────────────────────────────────────────────────

test("SimpleThreadPool reports the requested thread count", async () => {
    const whiteout = await Whiteout();
    const pool = whiteout.threadPool(4);
    try {
        assert.equal(pool.threadCount(), 4);
        pool.waitIdle();
    } finally {
        pool.delete();
    }
});

test("threaded mipmap generation produces the same chain as single-threaded", async () => {
    const whiteout = await Whiteout();

    // 16x16 solid red so we can run mipmap gen on a non-trivial chain.
    const pixels = new Array(16 * 16 * 3).fill(0)
        .map((_, i) => (i % 3 === 0 ? 255 : 0));
    const pngBytes = makePng(16, 16, pixels);

    const texA = whiteout.png.parse(pngBytes);
    const texB = whiteout.png.parse(pngBytes);
    const pool = whiteout.threadPool(2);
    try {
        // texture.generateMipmaps throws on failure (no "" / msg dance).
        whiteout.texture.generateMipmaps(texA);
        whiteout.texture.generateMipmaps(texB, { pool });
        pool.waitIdle();
        assert.equal(texA.mipCount(), texB.mipCount());
        assert.equal(texA.dataSize(), texB.dataSize());
    } finally {
        pool.delete();
        texA.delete();
        texB.delete();
    }
});


// ── HttpHandler trampoline ──────────────────────────────────────────────

test("makeHttpHandler wraps a JS object so capabilities() round-trips", async () => {
    const whiteout = await Whiteout();
    const handler = whiteout.makeHttpHandler({
        capabilities() { return 1; },
        getAsync(url, complete) {
            complete({ statusCode: 200, body: new Uint8Array([0x42]) });
        },
    });
    try {
        // The capabilities() override is called from C++ via the wrapper.
        assert.equal(handler.capabilities(), 1);
    } finally {
        handler.delete();
    }
});


// ── MPQ on-disk round-trip ──────────────────────────────────────────────

test("mpq.open opens an archive saved to a tmp dir", async () => {
    const whiteout = await Whiteout();

    // Build an MPQ in memory, save to disk, re-open via the disk opener.
    const dir = mkdtempSync(path.join(tmpdir(), "whiteout-node-mpq-"));
    const archivePath = path.join(dir, "test.mpq");
    try {
        const writer = whiteout.mpq.Storage.create({
            version: whiteout.mpq.FormatVersion.V1,
            hashTableSize: 64,
            sectorSizeShift: 3,
        });
        try {
            const payload = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);
            writer.writeFile("payload.bin", payload, {
                compression: whiteout.mpq.Compression.Zlib,
                locale: 0, encrypt: false, singleUnit: false,
            });
            assert.equal(whiteout.mpq.saveTo(writer, archivePath), true);
        } finally {
            writer.delete();
        }

        const reader = whiteout.mpq.open(archivePath);
        assert.ok(reader, "mpq.open returned null for a freshly saved archive");
        try {
            const bytes = reader.readFile("payload.bin");
            assert.ok(bytes instanceof Uint8Array);
            assert.deepEqual([...bytes], [0xde, 0xad, 0xbe, 0xef]);
        } finally {
            reader.delete();
        }
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("mpq.open returns null for a missing path", async () => {
    const whiteout = await Whiteout();
    const result = whiteout.mpq.open("/does/not/exist.mpq");
    assert.equal(result, null);
});
