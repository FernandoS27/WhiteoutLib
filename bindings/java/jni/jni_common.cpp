// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "jni_common.h"

namespace whiteout::jni {

JavaVM* g_jvm = nullptr;

AttachedEnv::AttachedEnv() {
    if (g_jvm == nullptr) return;
    // Fast path: thread already attached.
    void* env_ptr = nullptr;
    jint rc = g_jvm->GetEnv(&env_ptr, JNI_VERSION_1_6);
    if (rc == JNI_OK) {
        m_env = reinterpret_cast<JNIEnv*>(env_ptr);
        return;
    }
    // Slow path: attach. Library worker threads (CASC fetchers, MPQ
    // decompressors, etc.) typically aren't attached at startup.
    if (rc == JNI_EDETACHED) {
        rc = g_jvm->AttachCurrentThreadAsDaemon(&env_ptr, nullptr);
        if (rc == JNI_OK) {
            m_env = reinterpret_cast<JNIEnv*>(env_ptr);
            m_attached = true;
        }
    }
}

AttachedEnv::~AttachedEnv() {
    if (m_attached && g_jvm != nullptr) {
        g_jvm->DetachCurrentThread();
    }
}

std::vector<u8> jbyteArrayToVec(JNIEnv* env, jbyteArray arr) {
    if (env == nullptr || arr == nullptr) return {};
    const jsize len = env->GetArrayLength(arr);
    if (len <= 0) return {};
    std::vector<u8> out(static_cast<size_t>(len));
    env->GetByteArrayRegion(arr, 0, len,
                            reinterpret_cast<jbyte*>(out.data()));
    return out;
}

jbyteArray vecToJbyteArray(JNIEnv* env, const std::vector<u8>& v) {
    if (env == nullptr) return nullptr;
    const jsize len = static_cast<jsize>(v.size());
    jbyteArray arr = env->NewByteArray(len);
    if (arr == nullptr) return nullptr;  // OOM — exception pending
    if (len > 0) {
        env->SetByteArrayRegion(arr, 0, len,
            reinterpret_cast<const jbyte*>(v.data()));
    }
    return arr;
}

std::string jstringToString(JNIEnv* env, jstring s) {
    if (env == nullptr || s == nullptr) return {};
    const char* utf = env->GetStringUTFChars(s, nullptr);
    if (utf == nullptr) return {};
    std::string out(utf);
    env->ReleaseStringUTFChars(s, utf);
    return out;
}

jstring stringToJstring(JNIEnv* env, const std::string& s) {
    if (env == nullptr) return nullptr;
    return env->NewStringUTF(s.c_str());
}

bool consumePendingException(JNIEnv* env) {
    if (env == nullptr) return false;
    if (env->ExceptionCheck() == JNI_FALSE) return false;
    // Surfacing the throwable to System.err mirrors what the JVM does
    // for uncaught exceptions on the main thread; better than silent loss.
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

} // namespace whiteout::jni

// Captured once when the JVM loads this .dll (System.loadLibrary call
// in Java triggers it). Subsequent threads use g_jvm to attach.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    whiteout::jni::g_jvm = vm;
    return JNI_VERSION_1_6;
}
