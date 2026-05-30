// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using System.Text;
using Whiteout.Host;
using Xunit;

namespace Whiteout.Tests;

/// <summary>
/// Round-trip tests for the C# → C++ → C# trampoline on
/// <see cref="HttpHandler"/>. A managed subclass plugs into a C++ shim;
/// the smoke-test invokers drive <c>GetAsync</c> / <c>GetRangeAsync</c>
/// synchronously and capture the response the managed side fired back.
/// </summary>
public sealed class HttpHandlerTrampolineTest
{
    [Fact]
    public void Construct_And_Dispose()
    {
        using var handler = new StubHandler();
        Assert.False(handler.IsInvalid);
    }

    [Fact]
    public void Capabilities_RoundTripsThroughTheTrampoline()
    {
        using var handler = new StubHandler { CapabilitiesValue = HttpCapabilities.Http2Multiplexing };
        var caps = handler.InvokeCapabilitiesViaTrampoline();
        Assert.Equal((uint)HttpCapabilities.Http2Multiplexing, caps);
    }

    [Fact]
    public void GetAsync_FiresCallbackWithBodyAndStatus()
    {
        var expectedBody = Encoding.UTF8.GetBytes("hello world");
        using var handler = new StubHandler
        {
            OnGetAsync = (url, cb) =>
            {
                Assert.Equal("https://example.com/manifest", url);
                cb(HttpResponse.Ok(expectedBody));
            }
        };

        var response = handler.InvokeGetAsyncViaTrampoline("https://example.com/manifest");

        Assert.Equal(200, response.StatusCode);
        Assert.Equal(expectedBody, response.Body);
        Assert.Empty(response.Error);
    }

    [Fact]
    public void GetAsync_FiresCallbackWithFailure()
    {
        using var handler = new StubHandler
        {
            OnGetAsync = (url, cb) => cb(HttpResponse.Failure("network unreachable"))
        };

        var response = handler.InvokeGetAsyncViaTrampoline("https://example.com/x");

        Assert.Equal(0, response.StatusCode);
        Assert.Empty(response.Body);
        Assert.Equal("network unreachable", response.Error);
    }

    [Fact]
    public void GetRangeAsync_PassesRangeParamsThrough()
    {
        ulong gotStart = 0, gotEnd = 0;
        using var handler = new StubHandler
        {
            OnGetRangeAsync = (url, start, end, cb) =>
            {
                gotStart = start; gotEnd = end;
                cb(HttpResponse.Ok(new byte[] { 0xAA, 0xBB }));
            }
        };

        var response = handler.InvokeGetRangeAsyncViaTrampoline("https://example.com/file", 1000, 2047);

        Assert.Equal(1000ul, gotStart);
        Assert.Equal(2047ul, gotEnd);
        Assert.Equal(200, response.StatusCode);
        Assert.Equal(new byte[] { 0xAA, 0xBB }, response.Body);
    }

    [Fact]
    public void Callback_Single_Shot_SecondInvokeIsNoOp()
    {
        // The bridge swallows a second .Invoke — proves we never double-
        // delete the heap-allocated std::function on the C++ side.
        using var handler = new StubHandler
        {
            OnGetAsync = (url, cb) =>
            {
                cb(HttpResponse.Ok(new byte[] { 1, 2, 3 }));
                cb(HttpResponse.Ok(new byte[] { 4, 5, 6 }));   // second call: no-op
                cb(HttpResponse.Failure("late failure"));       // third call: no-op
            }
        };
        var response = handler.InvokeGetAsyncViaTrampoline("x");
        Assert.Equal(200, response.StatusCode);
        Assert.Equal(new byte[] { 1, 2, 3 }, response.Body);
    }

    [Fact]
    public void ManagedException_FiresCancelResponse_NoHang()
    {
        using var handler = new StubHandler
        {
            OnGetAsync = (url, cb) => throw new InvalidOperationException("boom")
        };

        // The library calling site would block forever if we never fired
        // the callback. Our trampoline catches the managed exception and
        // calls _cancel — which fires the C++ std::function with a clean
        // transport error. The smoke-test invoker round-trips that here.
        var response = handler.InvokeGetAsyncViaTrampoline("x");
        Assert.Equal(0, response.StatusCode);
        Assert.Contains("cancelled", response.Error, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Callback_FiredFromAnotherThread_StillWorks()
    {
        // Library guarantee: HttpHandler implementations may fire the
        // callback from any thread. Prove the bridge survives that.
        using var handler = new StubHandler
        {
            OnGetAsync = (url, cb) =>
            {
                var done = new ManualResetEventSlim();
                _ = Task.Run(() =>
                {
                    cb(HttpResponse.Ok(new byte[] { 0xFF }));
                    done.Set();
                });
                done.Wait();
            }
        };
        var response = handler.InvokeGetAsyncViaTrampoline("x");
        Assert.Equal(200, response.StatusCode);
        Assert.Equal(new byte[] { 0xFF }, response.Body);
    }

    /// <summary>HTTP handler stub that defers to caller-supplied callbacks
    /// per request type. Lets each test fully control the response.</summary>
    private sealed class StubHandler : HttpHandler
    {
        public HttpCapabilities CapabilitiesValue { get; set; } = HttpCapabilities.None;
        public Action<string, Action<HttpResponse>>? OnGetAsync { get; set; }
        public Action<string, ulong, ulong, Action<HttpResponse>>? OnGetRangeAsync { get; set; }

        public override uint Capabilities => (uint)CapabilitiesValue;

        public override void GetAsync(string url, Action<HttpResponse> callback)
            => (OnGetAsync ?? ((_, cb) => cb(HttpResponse.Failure("no handler set"))))(url, callback);

        public override void GetRangeAsync(string url, ulong start, ulong end, Action<HttpResponse> callback)
            => (OnGetRangeAsync ?? ((_, _, _, cb) => cb(HttpResponse.Failure("no handler set"))))(url, start, end, callback);
    }
}
