// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Runtime.InteropServices;

namespace Whiteout.Common;

/// <summary>
/// Base class for owning native handles produced by WhiteoutLib.
/// </summary>
/// <remarks>
/// <para>
/// Subclasses implement <see cref="ReleaseHandle"/> by calling the
/// appropriate <c>whiteout_&lt;Class&gt;_delete</c> entry point.
/// </para>
/// <para>
/// Borrowed handles — slices into a parent's memory — should NOT derive
/// from this class. Use a <c>readonly struct</c> view with
/// <see cref="System.IntPtr"/> field instead, since closing a borrowed
/// slice would double-free.
/// </para>
/// </remarks>
public abstract class WhiteoutHandle : SafeHandle
{
    /// <summary>Wraps an already-acquired native handle. <paramref name="owned"/>
    /// controls whether <c>Dispose</c> calls the native <c>_delete</c> entry
    /// point. Pass <c>false</c> for borrowed handles — pointers into a parent
    /// container's storage (e.g. an element returned by <c>get_field_at</c>)
    /// whose lifetime is bound to the parent.</summary>
    protected WhiteoutHandle(IntPtr handle, bool owned = true)
        : base(IntPtr.Zero, ownsHandle: owned)
    {
        SetHandle(handle);
    }

    /// <summary>Default ctor for P/Invoke marshalling; immediately set the handle.</summary>
    protected WhiteoutHandle() : base(IntPtr.Zero, ownsHandle: true) { }

    public override bool IsInvalid => handle == IntPtr.Zero;

    /// <summary>
    /// Returns the raw pointer for native calls. Throws
    /// <see cref="ObjectDisposedException"/> if the handle has been closed.
    /// </summary>
    internal IntPtr DangerousGet()
    {
        if (IsClosed || IsInvalid)
        {
            throw new ObjectDisposedException(GetType().Name);
        }
        return handle;
    }
}
