// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN (not codegen). Progress reporting for CASC opens — the C++
// callback is a std::function taking a struct, which the codegen can't
// marshal, so it goes through the whiteout_casc_shim_*Progress* shims.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Whiteout.Casc.Internal;
using Whiteout.Host;

namespace Whiteout.Casc;

/// <summary>Stage of the open sequence an event belongs to.</summary>
public enum CascProgressStep
{
    /// <summary>Online: versions/cdns endpoint lookup.</summary>
    ResolvingVersion = 0,
    /// <summary>Build config (fetch or disk read + parse).</summary>
    LoadingBuildConfig = 1,
    /// <summary>CDN config (fetch or disk read + parse).</summary>
    LoadingCdnConfig = 2,
    /// <summary>Local .idx bucket files.</summary>
    LoadingIndexFiles = 3,
    /// <summary>Local data.NNN archives being memory-mapped.</summary>
    MappingArchives = 4,
    /// <summary>Online: per-archive .index fetches.</summary>
    LoadingArchiveIndexes = 5,
    /// <summary>ENCODING manifest decode + parse.</summary>
    LoadingEncodingTable = 6,
    /// <summary>TVFS sub-manifests (~870 on WoW retail).</summary>
    LoadingVfsManifests = 7,
    /// <summary>ROOT manifest decode + parse.</summary>
    LoadingRootManifest = 8,
    /// <summary>Storage is usable. Always the final event.</summary>
    Ready = 9,
}

/// <summary>Position of an event within its step's lifetime.</summary>
public enum CascProgressState
{
    /// <summary>The step is starting. Always paired with an End.</summary>
    Begin = 0,
    /// <summary>Counters advanced. Throttled — samples may be dropped.</summary>
    Update = 1,
    /// <summary>The step finished; the counters hold the final tally.</summary>
    End = 2,
}

/// <summary>A single progress event.</summary>
public readonly struct CascProgressInfo
{
    /// <summary>Stage this event belongs to.</summary>
    public CascProgressStep Step { get; init; }
    /// <summary>Where in the step's lifetime this event sits.</summary>
    public CascProgressState State { get; init; }
    /// <summary>Name of the thing being worked on (archive key, filename,
    /// "ENCODING"); empty when the step has nothing to name.</summary>
    public string Object { get; init; }
    /// <summary>Items processed in this step.</summary>
    public ulong Current { get; init; }
    /// <summary>Items in this step; 0 when unknown.</summary>
    public ulong Total { get; init; }
    /// <summary>Bytes transferred in this step; 0 when untracked.</summary>
    public ulong BytesDone { get; init; }
    /// <summary>Expected bytes for this step; 0 when unknown.</summary>
    public ulong BytesTotal { get; init; }
    /// <summary>Position of this step in the planned sequence.</summary>
    public uint StepIndex { get; init; }
    /// <summary>Steps planned for this operation.</summary>
    public uint StepCount { get; init; }
    /// <summary>Milliseconds since the operation started.</summary>
    public double ElapsedMs { get; init; }
    /// <summary>Completion of the whole operation in [0,1].</summary>
    public double OverallFraction { get; init; }

    /// <summary>English label for <see cref="Step"/>.</summary>
    public string StepName => StorageProgress.StepName(Step);
}

/// <summary>Progress callback. Return false to cancel the operation.
/// Invoked from whichever thread does the work — including worker-pool
/// threads — but never concurrently.</summary>
public delegate bool CascProgressHandler(in CascProgressInfo info);

/// <summary>Opening CASC storage with progress reporting.</summary>
public static class StorageProgress
{
    /// <summary>Open a local storage, reporting progress as it goes.</summary>
    /// <param name="path">Game directory or its Data subdirectory.</param>
    /// <param name="progress">Callback; null opens without reporting.</param>
    /// <param name="product">Optional product code ("w3", "wowt", ...).</param>
    /// <param name="localeMask">Locale filter (0 = accept all).</param>
    /// <param name="flags">StorageFeatureFlags bitmask.</param>
    /// <param name="pool">Optional worker pool for parallel I/O.</param>
    /// <returns>A valid <see cref="Storage"/>, or null on failure or cancellation.</returns>
    public static Storage? OpenLocal(
        string path,
        CascProgressHandler? progress = null,
        string? product = null,
        uint localeMask = 0,
        uint flags = 0,
        WorkerPool? pool = null)
    {
        using var ctx = Context.Create(progress);
        IntPtr handle = NativeMethods.whiteout_casc_shim_openWithProgress(
            path,
            product ?? string.Empty,
            localeMask,
            flags,
            ctx.FunctionPointer,
            ctx.UserData,
            pool?.DangerousGet() ?? IntPtr.Zero);
        ctx.ThrowIfFaulted();
        return handle == IntPtr.Zero ? null : new Storage(handle);
    }

    /// <summary>Open a CDN-backed storage, reporting progress as it goes.</summary>
    /// <param name="product">Product code ("wow", "w3", "d3", ...).</param>
    /// <param name="region">Region for version lookup; empty defaults to "us".</param>
    /// <param name="http">HTTP transport; required.</param>
    /// <param name="progress">Callback; null opens without reporting.</param>
    /// <param name="buildKey">Optional hex build-config key.</param>
    /// <param name="cacheDir">Optional on-disk cache directory.</param>
    /// <param name="localeMask">Locale filter (0 = accept all).</param>
    /// <param name="flags">StorageFeatureFlags bitmask; 0 keeps the FullLazy default.</param>
    /// <param name="pool">Optional worker pool for parallel I/O.</param>
    /// <returns>A valid <see cref="Storage"/>, or null on failure or cancellation.</returns>
    public static Storage? OpenOnline(
        string product,
        string region,
        HttpHandler http,
        CascProgressHandler? progress = null,
        string? buildKey = null,
        string? cacheDir = null,
        uint localeMask = 0,
        uint flags = 0,
        WorkerPool? pool = null)
    {
        ArgumentNullException.ThrowIfNull(http);
        using var ctx = Context.Create(progress);
        IntPtr handle = NativeMethods.whiteout_casc_shim_openOnlineWithProgress(
            product,
            string.IsNullOrEmpty(region) ? "us" : region,
            buildKey ?? string.Empty,
            http.DangerousGet(),
            cacheDir ?? string.Empty,
            localeMask,
            flags,
            ctx.FunctionPointer,
            ctx.UserData,
            pool?.DangerousGet() ?? IntPtr.Zero);
        ctx.ThrowIfFaulted();
        return handle == IntPtr.Zero ? null : new Storage(handle);
    }

    /// <summary>Report progress for the work that happens after open: the
    /// deferred load a LoadOnDemand storage does on first access, and
    /// <c>Prefetch</c>. Each reports as its own operation. Pass null to stop.
    /// The handler is kept alive for as long as the storage is.</summary>
    public static void SetProgressCallback(this Storage storage, CascProgressHandler? progress)
    {
        ArgumentNullException.ThrowIfNull(storage);

        // The previous context is dropped only after the new one is installed,
        // so a callback in flight on a worker thread never sees a freed handle.
        Context? previous = s_installed.TryGetValue(storage, out var old) ? old : null;
        if (progress is null)
        {
            NativeMethods.whiteout_casc_shim_setProgressCallback(
                storage.DangerousGet(), IntPtr.Zero, IntPtr.Zero);
            s_installed.Remove(storage);
        }
        else
        {
            var ctx = Context.Create(progress);
            NativeMethods.whiteout_casc_shim_setProgressCallback(
                storage.DangerousGet(), ctx.FunctionPointer, ctx.UserData);
            s_installed.Remove(storage);
            s_installed.Add(storage, ctx);
        }
        previous?.Dispose();
    }

    /// <summary>English label for a step.</summary>
    public static string StepName(CascProgressStep step)
    {
        IntPtr p = NativeMethods.whiteout_casc_shim_progressStepName((int)step);
        return Marshal.PtrToStringUTF8(p) ?? step.ToString();
    }

    private static readonly ConditionalWeakTable<Storage, Context> s_installed = new();

    /// <summary>Native view of whiteout_casc_ProgressInfo.</summary>
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeProgressInfo
    {
        public uint Size;
        public int Step;
        public int State;
        public int Pad;
        public IntPtr Object;
        public ulong Current;
        public ulong Total;
        public ulong BytesDone;
        public ulong BytesTotal;
        public uint StepIndex;
        public uint StepCount;
        public double ElapsedMs;
        public double OverallFraction;
    }

    /// <summary>Keeps the managed handler reachable while native code holds a
    /// pointer to it, and carries any exception it threw back out.</summary>
    private sealed class Context : IDisposable
    {
        private GCHandle _self;
        public CascProgressHandler? Handler;
        public Exception? Fault;

        public static Context Create(CascProgressHandler? handler)
        {
            var ctx = new Context { Handler = handler };
            if (handler is not null)
                ctx._self = GCHandle.Alloc(ctx);
            return ctx;
        }

        public IntPtr UserData => _self.IsAllocated ? GCHandle.ToIntPtr(_self) : IntPtr.Zero;

        public IntPtr FunctionPointer
        {
            get
            {
                unsafe
                {
                    return Handler is null
                        ? IntPtr.Zero
                        : (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, IntPtr, int>)&Invoke;
                }
            }
        }

        public void ThrowIfFaulted()
        {
            if (Fault is not null)
                throw new InvalidOperationException("CASC progress callback threw", Fault);
        }

        public void Dispose()
        {
            if (_self.IsAllocated)
                _self.Free();
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
    private static unsafe int Invoke(IntPtr user, IntPtr infoPtr)
    {
        if (user == IntPtr.Zero || infoPtr == IntPtr.Zero)
            return 1;
        if (GCHandle.FromIntPtr(user).Target is not Context ctx || ctx.Handler is null)
            return 1;

        try
        {
            var native = *(NativeProgressInfo*)infoPtr;
            var info = new CascProgressInfo
            {
                Step = (CascProgressStep)native.Step,
                State = (CascProgressState)native.State,
                Object = Marshal.PtrToStringUTF8(native.Object) ?? string.Empty,
                Current = native.Current,
                Total = native.Total,
                BytesDone = native.BytesDone,
                BytesTotal = native.BytesTotal,
                StepIndex = native.StepIndex,
                StepCount = native.StepCount,
                ElapsedMs = native.ElapsedMs,
                OverallFraction = native.OverallFraction,
            };
            return ctx.Handler(in info) ? 1 : 0;
        }
        catch (Exception ex)
        {
            // An exception must not unwind through native frames. Remember it,
            // cancel the operation, and rethrow once the call returns.
            ctx.Fault ??= ex;
            return 0;
        }
    }
}
