// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Submit a Java Consumer<HttpResponse> to a NATIVE whiteout::interfaces::HttpHandler
// (getAsync / getRangeAsync). Mirror of native_pool_submit.cpp for HTTP.
//
// The Panama C ABI can't marshal std::function<void(HttpResponse)>, so
// the codegen-emitted `whiteout.utils.SimpleHttpHandler.getAsync` Java
// surface routes through these JNI shims. The handler receives a C++
// callback that bounces back into Java to invoke the user's Consumer.

#include <jni.h>

#include <cstdint>
#include <string>

#include <whiteout/interfaces.h>

#include "jni_common.h"

namespace {

// Lazy first-call init for the JVM identifiers we re-use on every fire.
jclass    g_consumer_class      = nullptr;
jmethodID g_consumer_accept     = nullptr;
jclass    g_httpresponse_class  = nullptr;
jmethodID g_httpresponse_ctor   = nullptr;

bool ensure_cached_ids(JNIEnv* env) {
    if (g_consumer_accept != nullptr && g_httpresponse_ctor != nullptr) return true;
    if (g_consumer_class == nullptr) {
        jclass cls = env->FindClass("java/util/function/Consumer");
        if (cls == nullptr) return false;
        g_consumer_class = static_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
        if (g_consumer_class == nullptr) return false;
        g_consumer_accept = env->GetMethodID(g_consumer_class, "accept", "(Ljava/lang/Object;)V");
        if (g_consumer_accept == nullptr) return false;
    }
    if (g_httpresponse_class == nullptr) {
        jclass cls = env->FindClass("whiteout/interfaces/HttpResponse");
        if (cls == nullptr) return false;
        g_httpresponse_class = static_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
        if (g_httpresponse_class == nullptr) return false;
        // record HttpResponse(int statusCode, byte[] body, String error)
        g_httpresponse_ctor = env->GetMethodID(g_httpresponse_class,
            "<init>", "(I[BLjava/lang/String;)V");
        if (g_httpresponse_ctor == nullptr) return false;
    }
    return true;
}

// Build the C++ HttpCallback lambda that bridges back into Java.
whiteout::interfaces::HttpCallback make_callback(jobject globalConsumer) {
    return [globalConsumer](whiteout::interfaces::HttpResponse resp) {
        whiteout::jni::AttachedEnv g;
        if (!g) {
            // Leak the global ref rather than crash; this thread can't
            // reach the JVM (shouldn't happen on SimpleHttpHandler's
            // own workers, which we control via JNI_OnLoad-cached JVM).
            return;
        }
        JNIEnv* e = g.env();
        // Convert std::vector<u8> body → Java byte[].
        jbyteArray jBody = whiteout::jni::vecToJbyteArray(e, resp.body);
        // Convert std::string error → Java String (nullable).
        jstring jError = whiteout::jni::stringToJstring(e, resp.error);
        // Build the HttpResponse record.
        jobject jResp = e->NewObject(g_httpresponse_class, g_httpresponse_ctor,
            static_cast<jint>(resp.statusCode), jBody, jError);
        if (jResp != nullptr) {
            e->CallVoidMethod(globalConsumer, g_consumer_accept, jResp);
            whiteout::jni::consumePendingException(e);
            e->DeleteLocalRef(jResp);
        }
        if (jBody != nullptr) e->DeleteLocalRef(jBody);
        if (jError != nullptr) e->DeleteLocalRef(jError);
        e->DeleteGlobalRef(globalConsumer);
    };
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_whiteout_utils_SimpleHttpHandler__1getAsync(
    JNIEnv* env, jclass /*cls*/, jlong handlerHandle, jstring url, jobject consumer)
{
    auto* handler = reinterpret_cast<whiteout::interfaces::HttpHandler*>(handlerHandle);
    if (handler == nullptr || consumer == nullptr) return;
    if (!ensure_cached_ids(env)) return;

    std::string cppUrl = whiteout::jni::jstringToString(env, url);
    jobject globalConsumer = env->NewGlobalRef(consumer);
    if (globalConsumer == nullptr) return;

    handler->getAsync(cppUrl, make_callback(globalConsumer));
}

JNIEXPORT void JNICALL
Java_whiteout_utils_SimpleHttpHandler__1getRangeAsync(
    JNIEnv* env, jclass /*cls*/, jlong handlerHandle, jstring url,
    jlong start, jlong end, jobject consumer)
{
    auto* handler = reinterpret_cast<whiteout::interfaces::HttpHandler*>(handlerHandle);
    if (handler == nullptr || consumer == nullptr) return;
    if (!ensure_cached_ids(env)) return;

    std::string cppUrl = whiteout::jni::jstringToString(env, url);
    jobject globalConsumer = env->NewGlobalRef(consumer);
    if (globalConsumer == nullptr) return;

    handler->getRangeAsync(cppUrl,
        static_cast< ::whiteout::u64>(start),
        static_cast< ::whiteout::u64>(end),
        make_callback(globalConsumer));
}

} // extern "C"
