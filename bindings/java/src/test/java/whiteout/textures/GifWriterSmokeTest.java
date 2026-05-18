// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Exercises the newly-bound gif::Writer surface, which depends on the
// codegen's vector<class-handle> param support: each Texture[] entry is
// marshalled as an opaque handle into an arena-allocated address array
// that the C++ side reconstructs into std::vector<Texture>.

package whiteout.textures;

public class GifWriterSmokeTest {

    public static void main(String[] args) {
        testWriteSingleFrame();
        testWriteMultipleFrames();
        testWriteEmptyFramesYieldsEmpty();
        testWriteWithSaveOptions();
        System.out.println("OK: all GifWriter smoke tests passed");
    }

    static void testWriteSingleFrame() {
        try (GifWriter writer = new GifWriter();
             Texture frame = Texture.create2D(PixelFormat.RGBA8, 32, 32, 1)) {
            byte[] gif = writer.write(new Texture[]{frame});
            require(gif != null && gif.length > 0,
                "single-frame write produces non-empty GIF bytes (got "
                + (gif == null ? "null" : gif.length) + ")");
            // GIF89a header magic.
            require(gif.length >= 6
                && gif[0] == 'G' && gif[1] == 'I' && gif[2] == 'F',
                "output starts with GIF magic");
        }
    }

    static void testWriteMultipleFrames() {
        try (GifWriter writer = new GifWriter();
             Texture a = Texture.create2D(PixelFormat.RGBA8, 32, 32, 1);
             Texture b = Texture.create2D(PixelFormat.RGBA8, 32, 32, 1);
             Texture c = Texture.create2D(PixelFormat.RGBA8, 32, 32, 1)) {
            byte[] gif = writer.write(new Texture[]{a, b, c});
            require(gif != null && gif.length > 0,
                "multi-frame write produces non-empty GIF bytes (got "
                + (gif == null ? "null" : gif.length) + ")");
        }
    }

    static void testWriteEmptyFramesYieldsEmpty() {
        try (GifWriter writer = new GifWriter()) {
            byte[] gif = writer.write(new Texture[0]);
            // Lenient mode (the default): empty input yields empty
            // output rather than throwing.
            require(gif != null, "empty input returns non-null byte[]");
            require(gif.length == 0,
                "empty input returns empty output (got " + gif.length + " bytes)");
        }
    }

    static void testWriteWithSaveOptions() {
        // Exercises the parser overload-disambiguation work — the
        // 4-arg `write(frames, opts)` overload now coexists with the
        // 2-arg `write(frames)` overload that used to shadow it.
        try (GifWriter writer = new GifWriter();
             GifSaveOptions opts = new GifSaveOptions();
             Texture a = Texture.create2D(PixelFormat.RGBA8, 32, 32, 1);
             Texture b = Texture.create2D(PixelFormat.RGBA8, 32, 32, 1)) {
            opts.setDelayCs((short) 20);     // 200 ms/frame
            opts.setLoopCount((short) 3);
            opts.setDither(true);
            opts.setDitherStrength(0.5f);
            byte[] gif = writer.write(new Texture[]{a, b}, opts);
            require(gif != null && gif.length > 0,
                "write(frames, opts) produces non-empty GIF bytes");
            require(gif.length >= 6
                && gif[0] == 'G' && gif[1] == 'I' && gif[2] == 'F',
                "GIF magic header still present in opts-overload output");
        }
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("FAIL: " + msg);
    }
}
