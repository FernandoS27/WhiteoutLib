// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Collections;
using System.Collections.Generic;

namespace Whiteout.Common;

/// <summary>
/// Lightweight <see cref="IReadOnlyList{T}"/> projection over a native
/// container exposed via a (count, at) C ABI pair.
/// </summary>
/// <remarks>
/// <para>Elements are fetched lazily — the wrapper holds the parent
/// <see cref="System.IntPtr"/> plus two delegates and yields fresh element
/// wrappers on each access. Elements returned by the indexer are typically
/// borrowed (non-owning) views into the parent container's storage; the
/// element factory should construct them with <c>owned: false</c> so the
/// parent retains ownership.</para>
///
/// <para>Mutating operations (insert, remove) are deliberately not exposed
/// here. The C ABI has matching `_resize` / `_set_at` pairs for that, which
/// the codegen can expose as explicit methods on the parent class.</para>
/// </remarks>
public sealed class NativeListView<T> : IReadOnlyList<T>
{
    private readonly IntPtr _parent;
    private readonly Func<IntPtr, nuint> _count;
    private readonly Func<IntPtr, nuint, T> _at;

    public NativeListView(IntPtr parent, Func<IntPtr, nuint> count, Func<IntPtr, nuint, T> at)
    {
        _parent = parent;
        _count = count;
        _at = at;
    }

    public int Count => checked((int)_count(_parent));

    public T this[int index]
    {
        get
        {
            if ((uint)index >= (uint)Count)
            {
                throw new ArgumentOutOfRangeException(nameof(index));
            }
            return _at(_parent, (nuint)index);
        }
    }

    public IEnumerator<T> GetEnumerator()
    {
        var n = Count;
        for (var i = 0; i < n; i++)
        {
            yield return _at(_parent, (nuint)i);
        }
    }

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
}
