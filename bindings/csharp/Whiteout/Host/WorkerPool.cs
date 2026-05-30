// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Whiteout.Common;
using Whiteout.Host.Internal;

namespace Whiteout.Host;

/// <summary>
/// Managed base for the C++ <c>whiteout::interfaces::WorkerPool</c>.
/// </summary>
/// <remarks>
/// Mirrors the dual-construction pattern of <see cref="VirtualPathFileSystem"/>:
/// the parameterless constructor builds a trampoline shim around a managed
/// subclass; the (IntPtr, bool) constructor wraps a native concrete impl
/// (<c>SimpleThreadPool</c>) without setting up a trampoline.
/// </remarks>
public abstract unsafe class WorkerPool : WhiteoutHandle
{
    [StructLayout(LayoutKind.Sequential)]
    private struct FnTable
    {
        public delegate* unmanaged<IntPtr, WorkerTaskFlat*, void> Submit;
        public delegate* unmanaged<IntPtr, void> WaitIdle;
        public delegate* unmanaged<IntPtr, nuint> ThreadCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WorkerTaskFlat
    {
        public IntPtr FnHandle;
        public IntPtr WaitSemaphore;
        public ulong  WaitValue;
        public IntPtr SignalSemaphore;
        public ulong  SignalValue;
    }

    private static readonly FnTable s_fnTable = new()
    {
        Submit      = &SubmitTrampoline,
        WaitIdle    = &WaitIdleTrampoline,
        ThreadCount = &ThreadCountTrampoline,
    };

    private GCHandle _self;

    /// <summary>Trampoline constructor — for user subclasses that
    /// implement the pool in managed code.</summary>
    protected WorkerPool()
    {
        _self = GCHandle.Alloc(this);
        fixed (FnTable* tablePtr = &Unsafe.AsRef(in s_fnTable))
        {
            var nativeHandle = NativeShims.whiteout_csharp_WorkerPool_create(
                GCHandle.ToIntPtr(_self), tablePtr);
            SetHandle(nativeHandle);
        }
    }

    /// <summary>Native-handle constructor — for codegen-generated concrete
    /// impls (<c>SimpleThreadPool</c>) that wrap an already-constructed
    /// C++ implementation.</summary>
    protected internal WorkerPool(IntPtr nativeHandle, bool owned) : base(nativeHandle, owned)
    {
    }

    /// <summary>Submit <paramref name="task"/> for execution. The pool
    /// must eventually call <see cref="WorkerTask.Run"/> on it.</summary>
    public abstract void Submit(WorkerTask task);

    /// <summary>Block until every submitted task has completed.</summary>
    public abstract void WaitIdle();

    /// <summary>The number of worker threads this pool dispatches across.</summary>
    public abstract ulong ThreadCount { get; }

    protected override bool ReleaseHandle()
    {
        if (handle != IntPtr.Zero && _self.IsAllocated)
        {
            NativeShims.whiteout_csharp_WorkerPool_delete(handle);
        }
        if (_self.IsAllocated)
        {
            _self.Free();
        }
        return true;
    }

    [UnmanagedCallersOnly]
    private static void SubmitTrampoline(IntPtr userdata, WorkerTaskFlat* taskPtr)
    {
        try
        {
            var instance = (WorkerPool)GCHandle.FromIntPtr(userdata).Target!;
            var task = new WorkerTask(
                taskPtr->FnHandle,
                taskPtr->WaitSemaphore, taskPtr->WaitValue,
                taskPtr->SignalSemaphore, taskPtr->SignalValue);
            instance.Submit(task);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Whiteout WorkerPool.Submit] {ex}");
            NativeShims.whiteout_csharp_WorkerTaskFn_cancel(taskPtr->FnHandle);
            if (taskPtr->SignalSemaphore != IntPtr.Zero)
            {
                NativeShims.whiteout_csharp_TimelineSemaphore_signal(
                    taskPtr->SignalSemaphore, taskPtr->SignalValue);
            }
        }
    }

    [UnmanagedCallersOnly]
    private static void WaitIdleTrampoline(IntPtr userdata)
    {
        try
        {
            var instance = (WorkerPool)GCHandle.FromIntPtr(userdata).Target!;
            instance.WaitIdle();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Whiteout WorkerPool.WaitIdle] {ex}");
        }
    }

    [UnmanagedCallersOnly]
    private static nuint ThreadCountTrampoline(IntPtr userdata)
    {
        try
        {
            var instance = (WorkerPool)GCHandle.FromIntPtr(userdata).Target!;
            return checked((nuint)instance.ThreadCount);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Whiteout WorkerPool.ThreadCount] {ex}");
            return 0;
        }
    }

    /// <summary>Drive the C++ virtual dispatch for <see cref="ThreadCount"/>
    /// regardless of how this pool was constructed.</summary>
    public ulong InvokeThreadCountViaTrampoline()
        => (ulong)NativeShims.whiteout_csharp_test_WorkerPool_threadCount(DangerousGet());

    /// <summary>Drive the C++ virtual dispatch for <see cref="WaitIdle"/>.</summary>
    public void InvokeWaitIdleViaTrampoline()
        => NativeShims.whiteout_csharp_test_WorkerPool_waitIdle(DangerousGet());

    /// <summary>Submit a task whose function increments
    /// <paramref name="sentinel"/>. Returns immediately; call
    /// <see cref="InvokeWaitIdleViaTrampoline"/> to ensure the task has
    /// run before reading the sentinel.</summary>
    public void InvokeSubmitIncrementSentinelViaTrampoline(ref int sentinel)
    {
        fixed (int* p = &sentinel)
        {
            NativeShims.whiteout_csharp_test_WorkerPool_submitIncrementSentinel(
                DangerousGet(), p);
        }
    }
}
