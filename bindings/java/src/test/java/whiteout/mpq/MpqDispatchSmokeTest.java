// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Closes the loop on the interface-dispatch design end-to-end through
// MPQ: build an in-memory archive with mpq.Storage.create(), wrap it in
// an mpq.FileSystem, and verify the FileSystem can be:
//   - used directly (its readFile/writeFile/fileExists go through the
//     Panama-emitted methods of the auto-bound MpqFileSystem class)
//   - dispatched as a whiteout.interfaces.VirtualPathFileSystem
//     (NativeHandled fast-path returns its underlying handle)

package whiteout.mpq;

import whiteout.host.NativeHandled;
import whiteout.host.VirtualPathFileSystems;

public class MpqDispatchSmokeTest {

    public static void main(String[] args) throws Exception {
        testCreateInMemoryArchive();
        testFileSystemWriteRead();
        testFileSystemAsInterface();
        testSaveToPathOverload();
        System.out.println("OK: all MPQ dispatch smoke tests passed");
    }

    static void testCreateInMemoryArchive() {
        try (CreateOptions opts = new CreateOptions();
             Storage storage = Storage.create(opts, /*pool=*/ null)) {
            require(storage != null, "Storage.create returns non-null");
            // Fresh archive shouldn't claim a file exists.
            require(!storage.fileExists("nope.txt"),
                "empty archive: fileExists(nope.txt) == false");
        }
    }

    static void testFileSystemWriteRead() {
        try (CreateOptions opts = new CreateOptions();
             Storage storage = Storage.create(opts, /*pool=*/ null);
             FileSystem fs = FileSystem.createStorage(storage)) {
            byte[] payload = "hello mpq".getBytes(java.nio.charset.StandardCharsets.UTF_8);
            boolean wrote = fs.writeFile("greet.txt", payload);
            require(wrote, "writeFile reports success");
            require(fs.fileExists("greet.txt"),
                "fileExists after writeFile");
            byte[] back = fs.readFile("greet.txt");
            require(java.util.Arrays.equals(back, payload),
                "readFile returns the same bytes (got " + back.length + " bytes)");
        }
    }

    static void testFileSystemAsInterface() {
        try (CreateOptions opts = new CreateOptions();
             Storage storage = Storage.create(opts, /*pool=*/ null);
             FileSystem fs = FileSystem.createStorage(storage)) {
            // FileSystem implements both interfaces; dispatch via the
            // host helper should hit the native fast-path.
            long expected = ((NativeHandled) fs).nativeHandle().address();
            long resolved = VirtualPathFileSystems.resolveNative(fs, fs);
            require(resolved == expected,
                "mpq.FileSystem fast-path: resolveNative returns the embedded handle");
            require(fs instanceof whiteout.interfaces.VirtualPathFileSystem,
                "mpq.FileSystem is-a whiteout.interfaces.VirtualPathFileSystem");
        }
    }

    static void testSaveToPathOverload() throws Exception {
        // Exercises the parser overload-disambiguation work — `save()` and
        // `save(path)` are now both bound. The path-variant was a noted
        // gap in tools/codegen/modules/mpq.py until the parser stopped
        // dropping all-but-one overload.
        java.nio.file.Path tmpDir = java.nio.file.Files.createTempDirectory("mpq-save");
        java.nio.file.Path outFile = tmpDir.resolve("fresh.mpq");
        try (CreateOptions opts = new CreateOptions();
             Storage storage = Storage.create(opts, /*pool=*/ null);
             FileSystem fs = FileSystem.createStorage(storage)) {
            byte[] payload = "saved-content".getBytes(java.nio.charset.StandardCharsets.UTF_8);
            require(fs.writeFile("data.bin", payload), "writeFile == true");
            require(storage.save(outFile.toString()),
                "save(path) reports success for fresh archive at " + outFile);
            require(java.nio.file.Files.size(outFile) > 0,
                "saved MPQ file is non-empty (size = "
                    + java.nio.file.Files.size(outFile) + ")");
        } finally {
            try (var s = java.nio.file.Files.walk(tmpDir)) {
                s.sorted(java.util.Comparator.reverseOrder())
                 .forEach(p -> { try { java.nio.file.Files.deleteIfExists(p); }
                                 catch (Exception ignored) {} });
            }
        }
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("FAIL: " + msg);
    }
}
