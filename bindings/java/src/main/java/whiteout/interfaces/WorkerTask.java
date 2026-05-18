// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

package whiteout.interfaces;

/**
 * Task descriptor handed to {@link WorkerPool#submit(WorkerTask)} by the JNI
 * bridge. Mirrors {@code whiteout::interfaces::WorkerTask} field-for-field:
 *
 * <ul>
 *   <li>{@code fn} — the work to run.</li>
 *   <li>{@code waitSemaphore} / {@code waitValue} — if set, the task must wait
 *       until {@code waitSemaphore.value() >= waitValue} before running.</li>
 *   <li>{@code signalSemaphore} / {@code signalValue} — if set, signal the
 *       semaphore to {@code signalValue} after the task completes.</li>
 * </ul>
 *
 * <p>{@link WorkerTask} implements {@link Runnable} with a default {@link #run()}
 * that honours the wait/signal contract. The simplest pool implementation is:
 *
 * <pre>{@code
 * public class SimplePool implements WorkerPool {
 *     private final ExecutorService exec = Executors.newFixedThreadPool(4);
 *     public void submit(WorkerTask task) { exec.submit(task); }
 * }
 * }</pre>
 *
 * <p>Power users that want dependency-aware scheduling can inspect
 * {@code task.waitSemaphore().value()} before deciding whether to run inline,
 * defer, or queue separately — see {@link TimelineSemaphore} for the lifetime
 * caveats.
 */
public record WorkerTask(
        Runnable fn,
        TimelineSemaphore waitSemaphore,
        long waitValue,
        TimelineSemaphore signalSemaphore,
        long signalValue)
    implements Runnable {

    /** Wait → fn → signal, honouring the semaphore contract. Null semaphores
     *  are skipped. */
    @Override
    public void run() {
        if (waitSemaphore != null) waitSemaphore.await(waitValue);
        fn.run();
        if (signalSemaphore != null) signalSemaphore.signal(signalValue);
    }
}
