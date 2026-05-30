// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout.Host.Internal;

namespace Whiteout.Host;

/// <summary>
/// A unit of work the C++ library handed to a managed <see cref="WorkerPool"/>.
/// Wraps the underlying <c>std::function&lt;void()&gt;</c> plus optional
/// timeline-semaphore coordination.
/// </summary>
/// <remarks>
/// <para>Call <see cref="Run"/> to execute the task — it waits on the
/// optional wait-semaphore, fires the underlying function, then signals
/// the optional signal-semaphore. <see cref="Run"/> is single-shot;
/// subsequent calls are no-ops.</para>
///
/// <para>If a managed pool decides to discard a task without running it
/// (shutdown, queue overflow, etc.), call <see cref="Discard"/> so the
/// heap-allocated function is freed. Note that discarding a task with a
/// signal semaphore will block library code waiting on that semaphore
/// forever — prefer to drain the queue rather than discard.</para>
/// </remarks>
public sealed class WorkerTask
{
    private IntPtr _fnHandle;
    private readonly IntPtr _waitSemaphore;
    private readonly ulong _waitValue;
    private readonly IntPtr _signalSemaphore;
    private readonly ulong _signalValue;
    private int _consumed;

    internal WorkerTask(IntPtr fnHandle, IntPtr waitSemaphore, ulong waitValue,
                        IntPtr signalSemaphore, ulong signalValue)
    {
        _fnHandle        = fnHandle;
        _waitSemaphore   = waitSemaphore;
        _waitValue       = waitValue;
        _signalSemaphore = signalSemaphore;
        _signalValue     = signalValue;
    }

    /// <summary>True when the task has a wait dependency that must be
    /// satisfied before its function can run.</summary>
    public bool HasWaitSemaphore => _waitSemaphore != IntPtr.Zero;

    /// <summary>True when the task signals downstream work after its function returns.</summary>
    public bool HasSignalSemaphore => _signalSemaphore != IntPtr.Zero;

    /// <summary>Execute the task synchronously: wait on the wait-semaphore
    /// (if any), invoke the C++ function, then signal the signal-semaphore
    /// (if any). Single-shot — re-entry is a no-op.</summary>
    public void Run()
    {
        if (Interlocked.CompareExchange(ref _consumed, 1, 0) != 0) return;
        var fn = Interlocked.Exchange(ref _fnHandle, IntPtr.Zero);
        if (fn == IntPtr.Zero) return;

        if (_waitSemaphore != IntPtr.Zero)
        {
            NativeShims.whiteout_csharp_TimelineSemaphore_await(_waitSemaphore, _waitValue);
        }
        NativeShims.whiteout_csharp_WorkerTaskFn_fire(fn);
        if (_signalSemaphore != IntPtr.Zero)
        {
            NativeShims.whiteout_csharp_TimelineSemaphore_signal(_signalSemaphore, _signalValue);
        }
    }

    /// <summary>Discard the task without running its function. Frees the
    /// C++ function object but never signals the signal-semaphore — only
    /// safe when the pool guarantees no library code is waiting on it.</summary>
    public void Discard()
    {
        if (Interlocked.CompareExchange(ref _consumed, 1, 0) != 0) return;
        var fn = Interlocked.Exchange(ref _fnHandle, IntPtr.Zero);
        if (fn == IntPtr.Zero) return;
        NativeShims.whiteout_csharp_WorkerTaskFn_cancel(fn);
    }
}
