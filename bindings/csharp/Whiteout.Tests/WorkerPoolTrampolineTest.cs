// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Collections.Concurrent;
using Whiteout.Host;
using Xunit;

namespace Whiteout.Tests;

/// <summary>
/// Round-trip tests for the C# → C++ → C# trampoline on
/// <see cref="WorkerPool"/>. The library submits a task with a
/// <c>std::function&lt;void()&gt;</c>; the managed pool dispatches it
/// and the task ultimately fires the C++ function back.
/// </summary>
public sealed class WorkerPoolTrampolineTest
{
    [Fact]
    public void Construct_And_Dispose()
    {
        using var pool = new ManagedPool(2);
        Assert.False(pool.IsInvalid);
    }

    [Fact]
    public void ThreadCount_RoundTrips()
    {
        using var pool = new ManagedPool(4);
        Assert.Equal((nuint)4, pool.InvokeThreadCountViaTrampoline());
    }

    [Fact]
    public void SubmittedTask_RunsThroughManagedPool_AndFiresCppFunction()
    {
        using var pool = new ManagedPool(2);
        int sentinel = 0;
        // C++ test helper submits a task whose function increments the sentinel.
        // For the increment to happen, the round-trip must succeed:
        //   C++ submit → C# trampoline → ManagedPool.Submit → enqueue
        //   → worker thread → WorkerTask.Run → C++ fn fire → sentinel++.
        pool.InvokeSubmitIncrementSentinelViaTrampoline(ref sentinel);
        pool.InvokeWaitIdleViaTrampoline();
        Assert.Equal(1, sentinel);
    }

    [Fact]
    public void MultipleTasks_AllRunBeforeWaitIdleReturns()
    {
        using var pool = new ManagedPool(4);
        int sentinel = 0;
        for (var i = 0; i < 50; i++)
        {
            pool.InvokeSubmitIncrementSentinelViaTrampoline(ref sentinel);
        }
        pool.InvokeWaitIdleViaTrampoline();
        Assert.Equal(50, sentinel);
    }

    [Fact]
    public void Submit_FromManagedException_DoesNotLeak_AndDoesNotHang()
    {
        // Pool that throws on Submit. The trampoline catches, cancels the
        // C++ function so it's freed, and (if there was a signal sem)
        // signals it. Library code won't deadlock.
        using var pool = new ThrowingPool();
        int sentinel = 0;
        pool.InvokeSubmitIncrementSentinelViaTrampoline(ref sentinel);
        pool.InvokeWaitIdleViaTrampoline();
        Assert.Equal(0, sentinel);  // Task never ran — managed Submit threw.
    }

    // ── Helper impls ─────────────────────────────────────────────────

    /// <summary>Managed pool backed by a fixed set of OS threads and a
    /// <see cref="BlockingCollection{T}"/>. Mirrors the SimpleThreadPool
    /// the C++ side ships as utils::SimpleThreadPool.</summary>
    private sealed class ManagedPool : WorkerPool
    {
        private readonly Thread[] _threads;
        private readonly BlockingCollection<WorkerTask> _queue = new();
        private int _inFlight;
        private readonly ManualResetEventSlim _idle = new(initialState: true);

        public ManagedPool(int threadCount)
        {
            _threads = new Thread[threadCount];
            for (var i = 0; i < threadCount; i++)
            {
                _threads[i] = new Thread(WorkerLoop) { IsBackground = true };
                _threads[i].Start();
            }
        }

        public override void Submit(WorkerTask task)
        {
            Interlocked.Increment(ref _inFlight);
            _idle.Reset();
            _queue.Add(task);
        }

        public override void WaitIdle()
        {
            _idle.Wait();
        }

        public override ulong ThreadCount => (ulong)_threads.Length;

        protected override bool ReleaseHandle()
        {
            // Drain + shut down workers before letting the base free the
            // native handle and GCHandle.
            _queue.CompleteAdding();
            foreach (var t in _threads) t.Join();
            return base.ReleaseHandle();
        }

        private void WorkerLoop()
        {
            foreach (var task in _queue.GetConsumingEnumerable())
            {
                try { task.Run(); }
                finally
                {
                    if (Interlocked.Decrement(ref _inFlight) == 0) _idle.Set();
                }
            }
        }
    }

    /// <summary>Pool whose Submit always throws — used to prove the
    /// trampoline's cancel + log path doesn't leak the std::function.</summary>
    private sealed class ThrowingPool : WorkerPool
    {
        public override void Submit(WorkerTask task)
            => throw new InvalidOperationException("simulated queue failure");

        public override void WaitIdle() { }
        public override ulong ThreadCount => 0;
    }
}
