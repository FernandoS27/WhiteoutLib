// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

namespace Whiteout.Host;

/// <summary>
/// The result of an HTTP request issued through <see cref="HttpHandler"/>.
/// </summary>
/// <param name="StatusCode">HTTP status code (200, 206, 404, ...). 0 when the
/// request failed before reaching the server.</param>
/// <param name="Body">Response body. Empty when the request failed or the
/// server returned no body.</param>
/// <param name="Error">Transport-level error message. Empty on success.</param>
public sealed record HttpResponse(int StatusCode, byte[] Body, string Error)
{
    /// <summary>Convenience constructor: <c>200 OK</c> with body bytes.</summary>
    public static HttpResponse Ok(byte[] body) => new(200, body, string.Empty);

    /// <summary>Convenience constructor: a transport error with no body.</summary>
    public static HttpResponse Failure(string error) =>
        new(0, Array.Empty<byte>(), error);
}

/// <summary>
/// Capability flags reported by an <see cref="HttpHandler"/> implementation.
/// Matches the C++ <c>whiteout::interfaces::HttpCapability</c> bitfield.
/// </summary>
[Flags]
public enum HttpCapabilities : uint
{
    None = 0,
    Http2Multiplexing = 0x1,
}
