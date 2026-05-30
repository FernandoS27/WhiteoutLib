// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Whiteout.Common;
using Whiteout.Host.Internal;

namespace Whiteout.Host;

/// <summary>
/// Managed base for the C++ <c>whiteout::interfaces::HttpHandler</c>.
/// </summary>
/// <remarks>
/// <para>Mirrors <see cref="VirtualPathFileSystem"/>: parameterless ctor
/// builds a trampoline shim for managed subclasses; (IntPtr, bool) ctor
/// wraps a native concrete impl (<see cref="SimpleHttpHandler"/>) without
/// a trampoline. The polymorphism lets either flow into APIs that expect
/// an <c>HttpHandler</c> (primarily <c>Casc.Storage.OpenOnline</c>).</para>
///
/// <para><b>Async contract</b> (managed subclasses only). Each callback
/// is heap-allocated on the C++ side and may be fired exactly once from
/// any thread. If the managed implementation throws before invoking the
/// callback the bridge fires a cancellation response with a transport
/// error so library code doesn't deadlock.</para>
/// </remarks>
public abstract unsafe class HttpHandler : WhiteoutHandle
{
    [StructLayout(LayoutKind.Sequential)]
    private struct FnTable
    {
        public delegate* unmanaged<IntPtr, uint> Capabilities;
        public delegate* unmanaged<IntPtr, byte*, nuint, IntPtr, void> GetAsync;
        public delegate* unmanaged<IntPtr, byte*, nuint, ulong, ulong, IntPtr, void> GetRangeAsync;
    }

    private static readonly FnTable s_fnTable = new()
    {
        Capabilities  = &CapabilitiesTrampoline,
        GetAsync      = &GetAsyncTrampoline,
        GetRangeAsync = &GetRangeAsyncTrampoline,
    };

    private GCHandle _self;

    /// <summary>Trampoline constructor.</summary>
    protected HttpHandler()
    {
        _self = GCHandle.Alloc(this);
        fixed (FnTable* tablePtr = &Unsafe.AsRef(in s_fnTable))
        {
            var nativeHandle = NativeShims.whiteout_csharp_HttpHandler_create(
                GCHandle.ToIntPtr(_self), tablePtr);
            SetHandle(nativeHandle);
        }
    }

    /// <summary>Native-handle constructor — for codegen-generated
    /// concrete impls (<see cref="SimpleHttpHandler"/>).</summary>
    protected internal HttpHandler(IntPtr nativeHandle, bool owned) : base(nativeHandle, owned)
    {
    }

    /// <summary>Capability flags as the C ABI's raw <c>u32</c> bitfield.
    /// Cast to <see cref="HttpCapabilities"/> for typed inspection.
    /// Defaults to <see cref="HttpCapabilities.None"/>.</summary>
    public virtual uint Capabilities => 0;

    /// <summary>Issue an HTTP GET for <paramref name="url"/>. Invoke
    /// <paramref name="callback"/> exactly once with the response.</summary>
    public abstract void GetAsync(string url, Action<HttpResponse> callback);

    /// <summary>Issue an HTTP range-GET for <paramref name="url"/>, byte
    /// range <c>[start, end]</c> inclusive. Invoke <paramref name="callback"/>
    /// exactly once.</summary>
    public abstract void GetRangeAsync(string url, ulong start, ulong end,
                                       Action<HttpResponse> callback);

    protected override bool ReleaseHandle()
    {
        if (handle != IntPtr.Zero && _self.IsAllocated)
        {
            NativeShims.whiteout_csharp_HttpHandler_delete(handle);
        }
        if (_self.IsAllocated)
        {
            _self.Free();
        }
        return true;
    }

    [UnmanagedCallersOnly]
    private static uint CapabilitiesTrampoline(IntPtr userdata)
    {
        try
        {
            var instance = (HttpHandler)GCHandle.FromIntPtr(userdata).Target!;
            return instance.Capabilities;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Whiteout HttpHandler.Capabilities] {ex}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    private static void GetAsyncTrampoline(IntPtr userdata, byte* urlPtr, nuint urlLen,
                                           IntPtr callbackHandle)
    {
        DispatchAsync(userdata, urlPtr, urlLen, callbackHandle,
            (instance, url, cb) => instance.GetAsync(url, cb),
            "GetAsync");
    }

    [UnmanagedCallersOnly]
    private static void GetRangeAsyncTrampoline(IntPtr userdata, byte* urlPtr, nuint urlLen,
                                                ulong start, ulong end, IntPtr callbackHandle)
    {
        DispatchAsync(userdata, urlPtr, urlLen, callbackHandle,
            (instance, url, cb) => instance.GetRangeAsync(url, start, end, cb),
            "GetRangeAsync");
    }

    private static void DispatchAsync(IntPtr userdata, byte* urlPtr, nuint urlLen,
                                      IntPtr callbackHandle,
                                      Action<HttpHandler, string, Action<HttpResponse>> dispatch,
                                      string methodName)
    {
        var bridge = new CallbackBridge(callbackHandle);
        try
        {
            var instance = (HttpHandler)GCHandle.FromIntPtr(userdata).Target!;
            var url = Encoding.UTF8.GetString(urlPtr, checked((int)urlLen));
            dispatch(instance, url, bridge.Fire);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Whiteout HttpHandler.{methodName}] {ex}");
            bridge.Cancel();
        }
    }

    /// <summary>Drive the C++ virtual dispatch for <see cref="Capabilities"/>
    /// regardless of how this handler was constructed.</summary>
    public uint InvokeCapabilitiesViaTrampoline()
        => NativeShims.whiteout_csharp_test_HttpHandler_capabilities(DangerousGet());

    /// <summary>Drive <see cref="GetAsync"/> via the native dispatch path,
    /// blocking until the managed handler fires the callback.</summary>
    public HttpResponse InvokeGetAsyncViaTrampoline(string url)
    {
        int status; NativeBytes body; NativeCString error;
        NativeShims.whiteout_csharp_test_HttpHandler_getAsync(
            DangerousGet(), url, &status, &body, &error);
        return new HttpResponse(status, body.ToManagedArray(), error.ToManagedString());
    }

    /// <summary>Drive <see cref="GetRangeAsync"/> via the native dispatch path.</summary>
    public HttpResponse InvokeGetRangeAsyncViaTrampoline(string url, ulong start, ulong end)
    {
        int status; NativeBytes body; NativeCString error;
        NativeShims.whiteout_csharp_test_HttpHandler_getRangeAsync(
            DangerousGet(), url, start, end, &status, &body, &error);
        return new HttpResponse(status, body.ToManagedArray(), error.ToManagedString());
    }

    /// <summary>Wraps a single C++ <c>std::function&lt;void(HttpResponse)&gt;</c>
    /// the trampoline received. First call to <see cref="Fire"/> or
    /// <see cref="Cancel"/> wins; subsequent calls are no-ops.</summary>
    private sealed class CallbackBridge
    {
        private IntPtr _handle;
        private int _fired;

        public CallbackBridge(IntPtr handle) { _handle = handle; }

        public void Fire(HttpResponse response)
        {
            if (Interlocked.CompareExchange(ref _fired, 1, 0) != 0) return;
            var handle = Interlocked.Exchange(ref _handle, IntPtr.Zero);
            if (handle == IntPtr.Zero) return;
            var body = response.Body ?? Array.Empty<byte>();
            var error = response.Error ?? string.Empty;
            unsafe
            {
                fixed (byte* bodyPtr = body)
                {
                    NativeShims.whiteout_csharp_HttpResponseCallback_fire(
                        handle, response.StatusCode, bodyPtr, (nuint)body.Length, error);
                }
            }
        }

        public void Cancel()
        {
            if (Interlocked.CompareExchange(ref _fired, 1, 0) != 0) return;
            var handle = Interlocked.Exchange(ref _handle, IntPtr.Zero);
            if (handle == IntPtr.Zero) return;
            NativeShims.whiteout_csharp_HttpResponseCallback_cancel(handle);
        }
    }
}
