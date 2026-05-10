import { Whiteout } from "../../packages/js-ts/index.js";
import { createHash } from "node:crypto";
import { deflateSync } from "node:zlib";

// Build a verified 1x1 PNG by computing CRCs from scratch.
function crc32(buf) {
    let c, table = [];
    for (let n = 0; n < 256; n++) {
        c = n;
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
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([typed, data])), 0);
    return Buffer.concat([len, typed, data, crc]);
}
const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(2, 0);  // width=2
ihdr.writeUInt32BE(2, 4);  // height=2
ihdr[8] = 8;  // bit depth
ihdr[9] = 2;  // RGB
// 0,0,0 already
// 4 RGB pixels (12 bytes), with filter byte 0x00 per scanline (2 scanlines, 7 bytes each)
const scanlines = Buffer.from([
    0x00, 0xff, 0x00, 0x00,  0x00, 0xff, 0x00, // filter + 2 RGB pixels (red, green) — wait I need to fix
]);
const raw = Buffer.from([0x00, 255,0,0, 0,255,0,  0x00, 0,0,255, 255,255,0]);
const idat = deflateSync(raw);
const png = Buffer.concat([sig, chunk("IHDR", ihdr), chunk("IDAT", idat), chunk("IEND", Buffer.alloc(0))]);
console.log("png len:", png.length, "first16:", png.slice(0,16).toString("hex"));

const wo = await Whiteout();
try {
    const tex = wo.png.parse(new Uint8Array(png));
    console.log("OK:", tex.width(), "x", tex.height(), "fmt:", tex.format());
    tex.delete();
} catch (e) {
    console.log("failed:", e?.constructor?.name);
}
