// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Progress reporting over the FFM upcall. A real CASC install is too large to
// ship, so this drives the handler over a directory that fails to open — which
// still announces the step it failed in, and is enough to prove events cross
// the boundary intact.

package whiteout.casc;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

public class CascProgressSmokeTest {

    public static void main(String[] args) throws IOException {
        stepNamesAreLabelled();
        openReportsEventsBeforeFailing();
        handlerCanCancel();
        handlerExceptionSurfaces();
        System.out.println("OK: all CASC progress smoke tests passed");
    }

    private static void stepNamesAreLabelled() {
        for (CascProgressStep step : CascProgressStep.values()) {
            String label = step.label();
            require(label != null && !label.isBlank(), "label for " + step);
        }
        require("Loading index files".equals(CascProgressStep.LoadingIndexFiles.label()),
            "LoadingIndexFiles is labelled by the library, not the enum name");
    }

    private static void openReportsEventsBeforeFailing() throws IOException {
        Path dir = Files.createTempDirectory("whiteout-java-progress");
        try {
            List<CascProgressInfo> events = new ArrayList<>();
            Optional<Storage> storage = CascShims.openWithProgress(
                dir.toString(), events::add, null, 0, 0, null);

            require(storage.isEmpty(), "a directory with no .idx must not open");
            require(!events.isEmpty(), "the failing step should still be named");

            boolean sawIndexStep = false;
            for (CascProgressInfo e : events) {
                require(e.stepCount() > 0, "every event carries a step plan");
                require(e.overallFraction() >= 0.0 && e.overallFraction() <= 1.0,
                    "overallFraction stays in [0,1] (got " + e.overallFraction() + ")");
                require(e.object() != null, "object is never null");
                if (e.step() == CascProgressStep.LoadingIndexFiles) sawIndexStep = true;
            }
            require(sawIndexStep, "the index-file step was announced");
        } finally {
            Files.deleteIfExists(dir);
        }
    }

    private static void handlerCanCancel() throws IOException {
        Path dir = Files.createTempDirectory("whiteout-java-progress-cancel");
        try {
            int[] calls = {0};
            Optional<Storage> storage = CascShims.openWithProgress(
                dir.toString(), info -> { calls[0]++; return false; }, null, 0, 0, null);

            require(storage.isEmpty(), "a cancelled open yields no storage");
            require(calls[0] == 1, "cancelling stops the event stream (got " + calls[0] + ")");
        } finally {
            Files.deleteIfExists(dir);
        }
    }

    private static void handlerExceptionSurfaces() throws IOException {
        Path dir = Files.createTempDirectory("whiteout-java-progress-throw");
        try {
            // A handler that throws must not unwind through the native frame:
            // the open is cancelled and the exception is rethrown here.
            boolean threw = false;
            try {
                CascShims.openWithProgress(dir.toString(), info -> {
                    throw new IllegalStateException("boom");
                }, null, 0, 0, null);
            } catch (IllegalStateException e) {
                threw = "boom".equals(e.getMessage());
            }
            require(threw, "a throwing handler surfaces its exception to the caller");
        } finally {
            Files.deleteIfExists(dir);
        }
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("FAIL: " + msg);
    }
}
