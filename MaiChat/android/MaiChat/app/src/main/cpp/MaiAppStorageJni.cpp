// 文件作用：Android 存储设置的 UTF-8/JNI 薄适配，候选与删除规则唯一实现位于共享核心。
#include <jni.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "MaiAppStorageBridge.h"

namespace {
std::string bytes(JNIEnv* env, jbyteArray input) {
    if (!input) return {};
    std::string result(static_cast<size_t>(env->GetArrayLength(input)), '\0');
    if (!result.empty()) env->GetByteArrayRegion(input, 0, result.size(),
        reinterpret_cast<jbyte*>(&result[0]));
    return result;
}
}
extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiAppStorageClient_nativeCreate(JNIEnv* env, jclass,
    jbyteArray temporary, jbyteArray cache, jbyteArray workspace) {
    const auto t = bytes(env, temporary), c = bytes(env, cache), w = bytes(env, workspace);
    return reinterpret_cast<intptr_t>(maiMobileAppStorageCreate(t.c_str(), c.c_str(), w.c_str()));
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_MaiAppStorageClient_nativeRequest(JNIEnv* env, jclass,
    jlong handle, jbyteArray input) {
    const auto request = bytes(env, input);
    char* response = maiMobileAppStorageRequest(reinterpret_cast<void*>(static_cast<intptr_t>(handle)), request.c_str());
    if (!response) return nullptr;
    auto result = env->NewByteArray(static_cast<jsize>(std::strlen(response)));
    if (result) env->SetByteArrayRegion(result, 0, env->GetArrayLength(result),
        reinterpret_cast<const jbyte*>(response));
    maiMobileAppStorageFree(response);
    return result;
}
extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiAppStorageClient_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    maiMobileAppStorageDestroy(reinterpret_cast<void*>(static_cast<intptr_t>(handle)));
}
