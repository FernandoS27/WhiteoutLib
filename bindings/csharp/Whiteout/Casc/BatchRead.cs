// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN (not codegen). Storage.readBatch takes a
// span<const BatchReadRequest> and returns a vector<BatchReadResult> —
// value objects in and out, which the codegen can't marshal. Goes through
// the whiteout_casc_shim_readBatch* shims in
// bindings/c/whiteout_casc_shims.cpp.

using System.Runtime.InteropServices;
using Whiteout.Casc.Internal;
using Whiteout.Common;

namespace Whiteout.Casc;

/// <summary>One file to read in a batch. Set <see cref="Path"/> to read by
/// CASC path, or leave it null and set <see cref="FileDataId"/> (plus
/// <see cref="Hint"/>) to read by WoW-style FileDataId.</summary>
public readonly struct BatchReadRequest
{
    public string? Path { get; init; }
    public int FileDataId { get; init; }
    public FileIdHint Hint { get; init; }

    public static BatchReadRequest ByPath(string path) => new() { Path = path };

    public static BatchReadRequest ById(int fileDataId, FileIdHint hint = FileIdHint.None)
        => new() { Path = null, FileDataId = fileDataId, Hint = hint };
}

/// <summary>Result of a single file in a batch read. <see cref="Data"/> is
/// null when <see cref="Success"/> is false; <see cref="Error"/> carries the
/// diagnostic in that case.</summary>
public readonly struct BatchReadResult
{
    public byte[]? Data { get; init; }
    public bool Success { get; init; }
    public string? Error { get; init; }
}

/// <summary>Batch file reads over a <see cref="Storage"/>. When the storage
/// was opened with a WorkerPool, resolution / raw read / BLTE decode overlap
/// across files — far faster than reading one file at a time.</summary>
public static class StorageBatchExtensions
{
    /// <summary>Read every requested file in one native call. Results come
    /// back in request order; an individual failure yields a result with
    /// <see cref="BatchReadResult.Success"/> false and does not affect the
    /// others.</summary>
    public static IReadOnlyList<BatchReadResult> ReadBatch(
        this Storage storage, IReadOnlyList<BatchReadRequest> requests)
    {
        ArgumentNullException.ThrowIfNull(storage);
        ArgumentNullException.ThrowIfNull(requests);

        int n = requests.Count;
        var output = new BatchReadResult[n];
        if (n == 0) return output;

        var paths = new IntPtr[n];
        var ids = new int[n];
        var hints = new int[n];
        try
        {
            for (int i = 0; i < n; i++)
            {
                BatchReadRequest r = requests[i];
                if (r.Path != null)
                {
                    paths[i] = Marshal.StringToCoTaskMemUTF8(r.Path);
                    ids[i] = -1;
                    hints[i] = 0;
                }
                else
                {
                    paths[i] = IntPtr.Zero;
                    ids[i] = r.FileDataId;
                    hints[i] = (int)r.Hint;
                }
            }

            IntPtr snapshot = NativeMethods.whiteout_casc_shim_readBatch(
                storage.DangerousGet(), paths, ids, hints, (nuint)n);
            if (snapshot == IntPtr.Zero) return output;

            try
            {
                int count = checked((int)NativeMethods.whiteout_casc_shim_readBatch_count(snapshot));
                for (int i = 0; i < count && i < n; i++)
                {
                    bool ok = NativeMethods.whiteout_casc_shim_readBatch_success_at(snapshot, (nuint)i) != 0;
                    byte[]? data = null;
                    if (ok)
                    {
                        NativeBytes nb = NativeMethods.whiteout_casc_shim_readBatch_data_at(snapshot, (nuint)i);
                        // Borrowed view (_owner == null) — copy without freeing;
                        // the whole snapshot is released below.
                        data = nb.ToManagedArray(freeAfter: false);
                    }
                    NativeCString err = NativeMethods.whiteout_casc_shim_readBatch_error_at(snapshot, (nuint)i);
                    output[i] = new BatchReadResult
                    {
                        Data = data,
                        Success = ok,
                        Error = err.ToManagedString(freeAfter: false),
                    };
                }
            }
            finally
            {
                NativeMethods.whiteout_casc_shim_readBatch_free(snapshot);
            }
        }
        finally
        {
            for (int i = 0; i < n; i++)
                if (paths[i] != IntPtr.Zero)
                    Marshal.FreeCoTaskMem(paths[i]);
        }

        return output;
    }
}
