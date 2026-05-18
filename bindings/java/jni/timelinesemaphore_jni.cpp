// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// JNI exports for whiteout.interfaces.TimelineSemaphore.
//
// The Java TimelineSemaphore is an opaque wrapper around a C++
// `whiteout::interfaces::TimelineSemaphore*`. Each instance is constructed
// by the bridge (with NewObject taking a `(J)V` ctor); these four native
// methods are the only way Java code can read or mutate the underlying
// semaphore.

#include <jni.h>

#include <cstdint>

#include <whiteout/interfaces.h>

#include "jni_common.h"

namespace {
using TimelineSemaphore = ::whiteout::interfaces::TimelineSemaphore;

TimelineSemaphore* as_sem(jlong handle) {
    return reinterpret_cast<TimelineSemaphore*>(handle);
}
} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_whiteout_interfaces_TimelineSemaphore__1await(JNIEnv*, jclass, jlong handle, jlong value) {
    auto* sem = as_sem(handle);
    if (sem == nullptr) return;
    sem->wait(static_cast<TimelineSemaphore::Value>(value));
}

JNIEXPORT void JNICALL
Java_whiteout_interfaces_TimelineSemaphore__1signal(JNIEnv*, jclass, jlong handle, jlong value) {
    auto* sem = as_sem(handle);
    if (sem == nullptr) return;
    sem->signal(static_cast<TimelineSemaphore::Value>(value));
}

JNIEXPORT jlong JNICALL
Java_whiteout_interfaces_TimelineSemaphore__1value(JNIEnv*, jclass, jlong handle) {
    auto* sem = as_sem(handle);
    return sem == nullptr ? 0 : static_cast<jlong>(sem->value());
}

JNIEXPORT jlong JNICALL
Java_whiteout_interfaces_TimelineSemaphore__1next(JNIEnv*, jclass, jlong handle) {
    auto* sem = as_sem(handle);
    return sem == nullptr ? 0 : static_cast<jlong>(sem->next());
}

} // extern "C"
