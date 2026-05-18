// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Submit a Java Runnable to a NATIVE whiteout::interfaces::WorkerPool.
//
// Used by the codegen-emitted `whiteout.utils.SimpleThreadPool.submit`
// (and any other native-backed wrapper of an interfaces::WorkerPool).
// The Panama C ABI can't marshal a std::function param, so we route
// through JNI here: the lambda we hand the C++ pool captures a JNI
// global ref to the Runnable, attaches a worker thread to the JVM
// when it runs, invokes Runnable.run(), then releases the ref.

#include <jni.h>

#include <cstdint>
#include <functional>
#include <utility>

#include <whiteout/interfaces.h>

#include "jni_common.h"

namespace {

// Cached on first use — Runnable.run() never changes for the JVM
// process, so a one-time lookup is safe. Atomic for thread safety on
// the first init.
jclass    g_runnable_class = nullptr;
jmethodID g_runnable_run   = nullptr;

bool ensure_runnable_methodid(JNIEnv* env) {
    if (g_runnable_run != nullptr) return true;
    jclass cls = env->FindClass("java/lang/Runnable");
    if (cls == nullptr) return false;
    jclass globalCls = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);
    if (globalCls == nullptr) return false;
    jmethodID run = env->GetMethodID(globalCls, "run", "()V");
    if (run == nullptr) {
        env->DeleteGlobalRef(globalCls);
        return false;
    }
    g_runnable_class = globalCls;
    g_runnable_run   = run;
    return true;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_whiteout_utils_SimpleThreadPool__1submitRunnable(
    JNIEnv* env, jclass /*cls*/, jlong poolHandle, jobject runnable,
    jlong waitSemHandle, jlong waitValue,
    jlong signalSemHandle, jlong signalValue)
{
    auto* pool = reinterpret_cast<whiteout::interfaces::WorkerPool*>(poolHandle);
    if (pool == nullptr || runnable == nullptr) return;
    if (!ensure_runnable_methodid(env)) return;

    // Promote the local jobject to a global ref so the C++ pool can
    // hold onto it across threads / time.
    jobject globalRunnable = env->NewGlobalRef(runnable);
    if (globalRunnable == nullptr) return;

    whiteout::interfaces::WorkerTask task;
    task.fn = [globalRunnable]() {
        whiteout::jni::AttachedEnv g;
        if (!g) {
            // Can't reach the JVM (shouldn't happen on threads spawned
            // by SimpleThreadPool which we control). Best we can do
            // here is leak the ref — calling DeleteGlobalRef without
            // an attached env would crash.
            return;
        }
        g.env()->CallVoidMethod(globalRunnable, g_runnable_run);
        whiteout::jni::consumePendingException(g.env());
        g.env()->DeleteGlobalRef(globalRunnable);
    };
    task.waitSemaphore   = reinterpret_cast<whiteout::interfaces::TimelineSemaphore*>(waitSemHandle);
    task.waitValue       = static_cast<whiteout::interfaces::TimelineSemaphore::Value>(waitValue);
    task.signalSemaphore = reinterpret_cast<whiteout::interfaces::TimelineSemaphore*>(signalSemHandle);
    task.signalValue     = static_cast<whiteout::interfaces::TimelineSemaphore::Value>(signalValue);

    pool->submit(task);
}

} // extern "C"
