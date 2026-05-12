// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Mirror of tests/python/test_smoke.py for the Java FFM bindings. Builds
// a 1×1 RGB PNG in memory, round-trips it through PngParser → Texture →
// PngWriter, and checks dimensions hold across the boundary.
//
// Run via scripts/build-java.ps1; the script stages whiteout_c.dll and
// the textures classes onto the classpath / native path before invoking.

package whiteout.textures;

import java.util.zip.CRC32;
import java.util.zip.Deflater;

public class SmokeTest {

    public static void main(String[] args) throws Exception {
        testPngRoundtrip();
        System.out.println("OK: all texture smoke tests passed");
    }

    static void testPngRoundtrip() throws Exception {
        byte[] src = makePng(1, 1, new byte[] { (byte) 255, 0, 0 });

        Texture parsed;
        try (PngParser parser = new PngParser()) {
            parsed = parser.parse(src).orElseThrow(() ->
                new AssertionError("parse returned empty"));
        }
        require(parsed.width() == 1,  "width == 1");
        require(parsed.height() == 1, "height == 1");

        byte[] encoded;
        try (PngWriter writer = new PngWriter()) {
            encoded = writer.write(parsed);
        } finally {
            parsed.close();
        }
        require(encoded != null && encoded.length > 0, "encoded non-empty");

        try (PngParser parser2 = new PngParser();
             Texture round = parser2.parse(encoded).orElseThrow()) {
            require(round.width() == 1,  "re-parsed width == 1");
            require(round.height() == 1, "re-parsed height == 1");
        }
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("smoke test failed: " + msg);
    }

    // ── Minimal PNG synthesis (no hand-rolled CRCs) ──────────────────────

    private static byte[] makePng(int width, int height, byte[] rgbPixels) {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        try {
            out.write(new byte[] {
                (byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'
            });
            byte[] ihdr = new byte[13];
            putInt(ihdr, 0, width);
            putInt(ihdr, 4, height);
            ihdr[8] = 8;  // bit depth
            ihdr[9] = 2;  // colour type = RGB
            // 10..12: compression, filter, interlace (all zero)
            writeChunk(out, "IHDR", ihdr);

            int stride = width * 3;
            byte[] raw = new byte[(stride + 1) * height];
            for (int y = 0; y < height; y++) {
                raw[y * (stride + 1)] = 0; // filter byte
                System.arraycopy(rgbPixels, y * stride,
                                 raw, y * (stride + 1) + 1, stride);
            }
            writeChunk(out, "IDAT", deflate(raw));
            writeChunk(out, "IEND", new byte[0]);
            return out.toByteArray();
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    private static void writeChunk(java.io.ByteArrayOutputStream out,
                                   String tag, byte[] data) throws Exception {
        byte[] len = new byte[4];
        putInt(len, 0, data.length);
        out.write(len);
        byte[] tagBytes = tag.getBytes("ASCII");
        out.write(tagBytes);
        out.write(data);
        CRC32 crc = new CRC32();
        crc.update(tagBytes);
        crc.update(data);
        byte[] c = new byte[4];
        putInt(c, 0, (int) crc.getValue());
        out.write(c);
    }

    private static byte[] deflate(byte[] input) {
        Deflater def = new Deflater();
        def.setInput(input);
        def.finish();
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[1024];
        while (!def.finished()) {
            int n = def.deflate(buf);
            out.write(buf, 0, n);
        }
        def.end();
        return out.toByteArray();
    }

    private static void putInt(byte[] b, int off, int v) {
        b[off]     = (byte) ((v >>> 24) & 0xFF);
        b[off + 1] = (byte) ((v >>> 16) & 0xFF);
        b[off + 2] = (byte) ((v >>>  8) & 0xFF);
        b[off + 3] = (byte) (v          & 0xFF);
    }
}
