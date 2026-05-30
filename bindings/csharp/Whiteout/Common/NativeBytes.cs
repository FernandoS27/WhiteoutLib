// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Whiteout.Common;

/// <summary>
/// Mirrors the C ABI's <c>whiteout_Bytes</c> struct. Returned by-value
/// from native parser functions; the bindings layer copies into a managed
/// <c>byte[]</c> (or projects as <see cref="ReadOnlySpan{T}"/>) and then
/// calls <c>whiteout_Bytes_free</c>.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeBytes
{
    public IntPtr Data;   // const uint8_t*
    public nuint Size;    // size_t
    public IntPtr Owner;  // void*   (opaque; passed back to whiteout_Bytes_free)

    public bool IsEmpty => Data == IntPtr.Zero;

    /// <summary>
    /// Copy contents into a managed array. <paramref name="freeAfter"/> calls
    /// <c>whiteout_Bytes_free</c> on the native owner; pass <c>false</c> only
    /// for borrowed buffers (<c>_owner == NULL</c>).
    /// </summary>
    public byte[] ToManagedArray(bool freeAfter = true)
    {
        if (IsEmpty)
        {
            if (freeAfter) NativeCommon.whiteout_Bytes_free(this);
            return Array.Empty<byte>();
        }

        var size = checked((int)Size);
        var result = new byte[size];
        if (size > 0)
        {
            unsafe
            {
                fixed (byte* dest = result)
                {
                    Unsafe.CopyBlock(dest, (void*)Data, (uint)size);
                }
            }
        }
        if (freeAfter) NativeCommon.whiteout_Bytes_free(this);
        return result;
    }
}

/// <summary>
/// Mirrors the C ABI's <c>whiteout_CString</c> struct.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeCString
{
    public IntPtr Chars;  // const char*
    public nuint Length;  // size_t
    public IntPtr Owner;  // void*

    public bool IsEmpty => Chars == IntPtr.Zero;

    public string ToManagedString(bool freeAfter = true)
    {
        if (IsEmpty)
        {
            if (freeAfter) NativeCommon.whiteout_CString_free(this);
            return string.Empty;
        }

        var len = checked((int)Length);
        string result;
        unsafe
        {
            result = Encoding.UTF8.GetString((byte*)Chars, len);
        }
        if (freeAfter) NativeCommon.whiteout_CString_free(this);
        return result;
    }
}
