// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN (not codegen). Mirrors whiteout_casc_ProgressInfo.

package whiteout.casc;

/**
 * A single progress event.
 *
 * <p>Every field except {@code step} and {@code state} is optional: a step that
 * cannot count its work leaves the counters at zero. {@code stepIndex} /
 * {@code stepCount} describe the whole operation, so a UI can draw one honest
 * bar without guessing how many phases are left.
 *
 * @param step            which phase this event belongs to
 * @param state           where in the phase's lifetime the event sits
 * @param object          what is being worked on — an archive key, a filename,
 *                        {@code "ENCODING"}; empty when there is nothing to name
 * @param current         items processed in this step
 * @param total           items in this step; 0 when unknown
 * @param bytesDone       bytes transferred in this step; 0 when untracked
 * @param bytesTotal      expected bytes for this step; 0 when unknown
 * @param stepIndex       position of {@code step} in the planned sequence
 * @param stepCount       steps planned for this operation
 * @param elapsedMs       milliseconds since the operation started
 * @param overallFraction completion of the whole operation, in {@code [0,1]}
 */
public record CascProgressInfo(CascProgressStep step,
                               CascProgressState state,
                               String object,
                               long current,
                               long total,
                               long bytesDone,
                               long bytesTotal,
                               int stepIndex,
                               int stepCount,
                               double elapsedMs,
                               double overallFraction) {

    /** Completion of this step alone, in {@code [0,1]}; 0 when it reports no counts. */
    public double stepFraction() {
        if (state == CascProgressState.End) return 1.0;
        if (total != 0) return (double) current / (double) total;
        if (bytesTotal != 0) return (double) bytesDone / (double) bytesTotal;
        return 0.0;
    }
}
