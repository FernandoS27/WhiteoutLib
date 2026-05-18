// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Shared JNI helpers for the whiteout_jni shared library.
//
// Each abstract interface in include/whiteout/interfaces.h has its own
// "JavaXxx" C++ bridge class that derives from the interface and
// forwards every virtual override into a Java object via JNI. They all
// share the same JVM-attach + reference-management + exception-handling
// machinery — that machinery lives in this file.

#pragma once

#include <jni.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::jni {

/// JavaVM pointer captured at JNI_OnLoad time. Library worker threads
/// call attachCurrentEnv() to obtain a JNIEnv* without crashing.
extern JavaVM* g_jvm;

/// RAII guard that attaches the calling thread to the JVM if it isn't
/// already attached, and detaches on destruction (only if WE attached;
/// pre-attached threads stay attached). Use this at the top of every
/// C++ override that's about to call back into Java.
class AttachedEnv {
public:
    AttachedEnv();
    ~AttachedEnv();

    AttachedEnv(const AttachedEnv&) = delete;
    AttachedEnv& operator=(const AttachedEnv&) = delete;

    JNIEnv* env() const noexcept { return m_env; }
    explicit operator bool() const noexcept { return m_env != nullptr; }

private:
    JNIEnv* m_env = nullptr;
    bool m_attached = false;  // true if WE attached (must detach on dtor)
};

/// Convert a Java byte[] to std::vector<u8>. Returns empty on null input.
std::vector<u8> jbyteArrayToVec(JNIEnv* env, jbyteArray arr);

/// Wrap a std::vector<u8> in a fresh Java byte[]. Returns nullptr on
/// allocation failure (JVM raised OutOfMemoryError).
jbyteArray vecToJbyteArray(JNIEnv* env, const std::vector<u8>& v);

/// Convert a Java String to std::string (UTF-8). Returns empty on null
/// input. The caller MUST check for a pending Java exception afterwards
/// when the input could be malformed.
std::string jstringToString(JNIEnv* env, jstring s);

/// Wrap a std::string as a Java String. Returns nullptr if env is null.
jstring stringToJstring(JNIEnv* env, const std::string& s);

/// If a Java exception is pending, log it (System.err.println via
/// Throwable.printStackTrace) and clear it so the caller can return.
/// Returns true if an exception was pending.
bool consumePendingException(JNIEnv* env);

} // namespace whiteout::jni
