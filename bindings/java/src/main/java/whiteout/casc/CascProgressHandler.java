// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

package whiteout.casc;

/**
 * Receives progress events while a CASC storage is being opened.
 *
 * <p>Invoked from whichever thread is doing the work — that includes native
 * worker-pool threads during the parallel index and manifest phases — but never
 * concurrently. It also never makes a worker wait: a slow handler costs dropped
 * {@link CascProgressState#Update} samples rather than throughput. Begin, End
 * and the terminal Ready event are always delivered.
 */
@FunctionalInterface
public interface CascProgressHandler {

    /**
     * @param info the event; valid only for the duration of this call
     * @return false to cancel the operation, which then fails with
     *         {@code CascError::Cancelled}
     */
    boolean onProgress(CascProgressInfo info);
}
