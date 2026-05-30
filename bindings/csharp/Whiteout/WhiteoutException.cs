// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

namespace Whiteout;

/// <summary>
/// Thrown for programming errors when crossing the native boundary
/// (out-of-range access on a custom optional, unexpected null handle, etc.).
/// </summary>
/// <remarks>
/// The underlying C++ library is built with exceptions disabled and never
/// throws on bad <i>input</i> — parser failure is surfaced via
/// <c>parser.Issues</c> and an empty result, not an exception. This type
/// is only used for programmer errors that the bindings layer catches.
/// </remarks>
public sealed class WhiteoutException : Exception
{
    public WhiteoutException(string message) : base(message) { }
    public WhiteoutException(string message, Exception innerException) : base(message, innerException) { }
}
