// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Exercises the now-implemented SimpleThreadPool.submit. Previously a
// stub that threw UnsupportedOperationException — now routed through
// the JNI shim in bindings/java/jni/native_pool_submit.cpp.

package whiteout.utils;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import whiteout.interfaces.WorkerTask;

public class SimpleThreadPoolSubmitSmokeTest {

    public static void main(String[] args) throws Exception {
        testSubmitsRunOnPoolThreads();
        testManySubmissionsAllRun();
        System.out.println("OK: all SimpleThreadPool submit smoke tests passed");
    }

    static void testSubmitsRunOnPoolThreads() throws Exception {
        try (SimpleThreadPool pool = SimpleThreadPool.createNThreads(2)) {
            CountDownLatch done = new CountDownLatch(1);
            AtomicInteger ranOn = new AtomicInteger();
            Runnable r = () -> {
                ranOn.set(1);
                done.countDown();
            };
            pool.submit(new WorkerTask(r, null, 0L, null, 0L));
            require(done.await(5, TimeUnit.SECONDS),
                "Runnable ran on a pool worker within 5s");
            require(ranOn.get() == 1, "Runnable's side effect visible");
            pool.waitIdle();
        }
    }

    static void testManySubmissionsAllRun() throws Exception {
        final int N = 64;
        try (SimpleThreadPool pool = SimpleThreadPool.createNThreads(4)) {
            CountDownLatch done = new CountDownLatch(N);
            AtomicInteger total = new AtomicInteger();
            for (int i = 0; i < N; ++i) {
                final int val = i;
                pool.submit(new WorkerTask(() -> {
                    total.addAndGet(val);
                    done.countDown();
                }, null, 0L, null, 0L));
            }
            require(done.await(10, TimeUnit.SECONDS),
                "all " + N + " runnables completed within 10s");
            // 0+1+...+63 = 2016
            require(total.get() == (N * (N - 1)) / 2,
                "sum matches expected 0..N-1 (got " + total.get() + ")");
            pool.waitIdle();
        }
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("FAIL: " + msg);
    }
}
