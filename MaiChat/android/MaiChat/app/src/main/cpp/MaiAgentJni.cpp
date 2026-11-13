// 文件作用：Android JNI 桥接层；MaiAgentJni 负责 Java 与原生引擎之间的参数和生命周期转换，本文件给出其实现。
// 直接依赖：MaiMobileAgent.h、MaiImageFilter.h、MaiAvatarClient.h；头像 HTTP 上传复用共享 C ABI。
#include "MaiMobileAgent.h"
#include "MaiImageFilter.h"
#include "MaiPortraitBridge.h"
#include "MaiAndroidFileOperations.h"
#include "MaiAvatarClient.h"
#ifndef NDEBUG
#include "MaiDownloadFileTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiTool.h"
#endif
#include <android/bitmap.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <memory>
#include <string>
#include <vector>

namespace {

JavaVM* sFileJavaVm = nullptr;
jclass sFileOperationsClass = nullptr;
jmethodID sPublishNewFileMethod = nullptr;
jclass sAssistantClass = nullptr;
jmethodID sAssistantChangedMethod = nullptr;

void publishAgentChangeToJava(const char* sessionId) {
    if (sessionId == nullptr || sAssistantClass == nullptr || sAssistantChangedMethod == nullptr)
        return;
    JNIEnv* env = nullptr;
    bool detachAfter = false;
    if (sFileJavaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (sFileJavaVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        detachAfter = true;
    }
    if (env != nullptr) {
        jstring value = env->NewStringUTF(sessionId);
        if (value != nullptr) {
            env->CallStaticVoidMethod(sAssistantClass, sAssistantChangedMethod, value);
            env->DeleteLocalRef(value);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (detachAfter) sFileJavaVm->DetachCurrentThread();
}

int publishNewFileInJava(const char* source, const char* destination) {
    JNIEnv* env = nullptr;
    bool detachAfter = false;
    if (sFileJavaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (sFileJavaVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return EIO;
        detachAfter = true;
    }
    if (env == nullptr) return EIO;

    const auto makeBytes = [env](const char* value) {
        const jsize length = static_cast<jsize>(std::strlen(value));
        jbyteArray bytes = env->NewByteArray(length);
        if (bytes != nullptr) env->SetByteArrayRegion(bytes, 0, length, reinterpret_cast<const jbyte*>(value));
        return bytes;
    };
    jbyteArray sourceBytes = makeBytes(source);
    jbyteArray destinationBytes = sourceBytes != nullptr ? makeBytes(destination) : nullptr;
    int status = ENOMEM;
    bool calledJava = false;
    if (sourceBytes != nullptr && destinationBytes != nullptr && !env->ExceptionCheck()) {
        calledJava = true;
        const jint result =
            env->CallStaticIntMethod(sFileOperationsClass, sPublishNewFileMethod, sourceBytes, destinationBytes);
        if (!env->ExceptionCheck()) {
            switch (result) {
                case 0: status = 0; break;
                case 1: status = EEXIST; break;
                case 2: status = ENOENT; break;
                case 3: status = EACCES; break;
                case 4: status = EINVAL; break;
                default: status = EIO; break;
            }
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        status = calledJava ? EIO : ENOMEM;
    }
    if (sourceBytes != nullptr) env->DeleteLocalRef(sourceBytes);
    if (destinationBytes != nullptr) env->DeleteLocalRef(destinationBytes);
    if (detachAfter) sFileJavaVm->DetachCurrentThread();
    return status;
}

struct JniHostToolContext {
  JavaVM *vm = nullptr;
  jobject owner = nullptr;
  jmethodID invoke = nullptr;
};

JNIEnv *attach(JniHostToolContext *context, bool *detachAfter) {
  *detachAfter = false;
  JNIEnv *env = nullptr;
  if (context->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) ==
      JNI_OK)
    return env;
  if (context->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
    return nullptr;
  *detachAfter = true;
  return env;
}

const char *hostToolCall(void *opaque, const char *toolName,
                         const char *argumentsJson) {
  auto *context = static_cast<JniHostToolContext *>(opaque);
  bool detachAfter = false;
  JNIEnv *env = attach(context, &detachAfter);
  if (env == nullptr)
    return nullptr;
  const auto makeBytes = [&](const char *value) {
    const jsize size = static_cast<jsize>(std::strlen(value));
    jbyteArray bytes = env->NewByteArray(size);
    if (bytes != nullptr)
      env->SetByteArrayRegion(bytes, 0, size,
                              reinterpret_cast<const jbyte *>(value));
    return bytes;
  };
  jbyteArray tool = makeBytes(toolName);
  jbyteArray arguments = makeBytes(argumentsJson);
  auto *result = static_cast<jbyteArray>(
      env->CallObjectMethod(context->owner, context->invoke, tool, arguments));
  env->DeleteLocalRef(tool);
  env->DeleteLocalRef(arguments);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    result = nullptr;
  }
  char *owned = nullptr;
  if (result != nullptr) {
    const jsize size = env->GetArrayLength(result);
    owned =
        static_cast<char *>(std::malloc(static_cast<std::size_t>(size) + 1));
    if (owned != nullptr) {
      env->GetByteArrayRegion(result, 0, size,
                              reinterpret_cast<jbyte *>(owned));
      owned[size] = '\0';
    }
    env->DeleteLocalRef(result);
  }
  if (detachAfter)
    context->vm->DetachCurrentThread();
  return owned;
}

void hostToolResponseFree(void *, const char *response) {
  std::free(const_cast<char *>(response));
}

void hostToolContextRelease(void *opaque) {
  auto *context = static_cast<JniHostToolContext *>(opaque);
  bool detachAfter = false;
  JNIEnv *env = attach(context, &detachAfter);
  if (env != nullptr)
    env->DeleteGlobalRef(context->owner);
  if (detachAfter)
    context->vm->DetachCurrentThread();
  delete context;
}

} // namespace

namespace {

bool readAvatarBytes(JNIEnv* env, jbyteArray input, std::size_t maximum,
                     std::vector<unsigned char>* output) {
    if (input == nullptr) return false;
    const jsize length = env->GetArrayLength(input);
    if (length < 0 || static_cast<std::size_t>(length) > maximum) return false;
    output->resize(static_cast<std::size_t>(length));
    if (length > 0)
        env->GetByteArrayRegion(input, 0, length, reinterpret_cast<jbyte*>(output->data()));
    return !env->ExceptionCheck();
}

bool readAvatarText(JNIEnv* env, jbyteArray input, std::size_t maximum, std::string* output) {
    std::vector<unsigned char> bytes;
    if (!readAvatarBytes(env, input, maximum, &bytes) || bytes.empty()) return false;
    output->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return output->find('\0') == std::string::npos;
}

void avatarFailure(JNIEnv* env, const char* message) {
    if (env->ExceptionCheck()) return;
    jclass type = env->FindClass("java/io/IOException");
    if (type != nullptr) {
        env->ThrowNew(type, message);
        env->DeleteLocalRef(type);
    }
}

struct AvatarProxyContext {
    JNIEnv* env = nullptr;
    jclass owner = nullptr;
    jmethodID select = nullptr;
    std::string route;
};

int resolveAvatarProxy(const char* url, void* opaque, MaiDownloadProxy* output) {
    auto* proxy = static_cast<AvatarProxyContext*>(opaque);
    jstring address = proxy->env->NewStringUTF(url);
    if (address == nullptr) return 0;
    auto selected = static_cast<jstring>(proxy->env->CallStaticObjectMethod(
        proxy->owner, proxy->select, address));
    proxy->env->DeleteLocalRef(address);
    if (proxy->env->ExceptionCheck()) {
        // 系统代理异常可能包含请求 URL；统一在头像入口返回固定错误，不向工具转发原文。
        proxy->env->ExceptionClear();
        return 0;
    }
    if (selected == nullptr) return 0;
    const char* route = proxy->env->GetStringUTFChars(selected, nullptr);
    if (route == nullptr) {
        proxy->env->DeleteLocalRef(selected);
        return 0;
    }
    proxy->route = route;
    proxy->env->ReleaseStringUTFChars(selected, route);
    proxy->env->DeleteLocalRef(selected);
    output->url_utf8 = proxy->route.c_str();
    output->username_utf8 = nullptr;
    output->password_utf8 = nullptr;
    return 1;
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiAccountAvatar_nativeCreateCancel(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(mai_avatar_cancel_create());
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiAccountAvatar_nativeRequestCancel(JNIEnv*, jclass, jlong handle) {
    if (handle != 0) mai_avatar_cancel_request(reinterpret_cast<MaiAvatarCancel*>(handle));
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiAccountAvatar_nativeDestroyCancel(JNIEnv*, jclass, jlong handle) {
    if (handle != 0) mai_avatar_cancel_destroy(reinterpret_cast<MaiAvatarCancel*>(handle));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_MaiAccountAvatar_nativeUpload(
    JNIEnv* env, jclass owner, jbyteArray base, jbyteArray token, jbyteArray accountId,
    jbyteArray caPem, jbyteArray jpeg, jlong cancel) {
    std::string baseText, tokenText, accountText;
    std::vector<unsigned char> pemBytes, imageBytes;
    if (cancel == 0 || !readAvatarText(env, base, 4096, &baseText) ||
        !readAvatarText(env, token, 16 * 1024, &tokenText) ||
        !readAvatarText(env, accountId, 256, &accountText) ||
        !readAvatarBytes(env, caPem, 2 * 1024 * 1024, &pemBytes) ||
        !readAvatarBytes(env, jpeg, 2'000'000, &imageBytes) ||
        pemBytes.empty() || imageBytes.empty()) {
        avatarFailure(env, "Avatar upload input is invalid");
        return nullptr;
    }
    AvatarProxyContext proxy;
    proxy.env = env;
    proxy.owner = owner;
    proxy.select = env->GetStaticMethodID(owner, "proxyForUrl",
        "(Ljava/lang/String;)Ljava/lang/String;");
    if (proxy.select == nullptr) return nullptr;
    MaiAvatarOptions options{};
    options.media_base_url_utf8 = baseText.c_str();
    options.account_token_utf8 = tokenText.c_str();
    options.account_id_utf8 = accountText.c_str();
    options.ca_pem = reinterpret_cast<const char*>(pemBytes.data());
    options.ca_pem_length = pemBytes.size();
    options.proxy_resolver = resolveAvatarProxy;
    options.proxy_context = &proxy;
    options.connect_timeout_ms = 10'000;
    options.transfer_timeout_ms = 60'000;
    MaiAvatarResult result{};
    mai_avatar_upload(&options, imageBytes.data(), imageBytes.size(), "image/jpeg",
        reinterpret_cast<const MaiAvatarCancel*>(cancel), &result);
    jbyteArray output = nullptr;
    if (result.status == MAI_AVATAR_OK && result.avatar_url_utf8 != nullptr) {
        const std::size_t length = std::strlen(result.avatar_url_utf8);
        if (length > 0 && length <= 8192) {
            output = env->NewByteArray(static_cast<jsize>(length));
            if (output != nullptr) env->SetByteArrayRegion(output, 0, static_cast<jsize>(length),
                reinterpret_cast<const jbyte*>(result.avatar_url_utf8));
        }
    }
    const bool canceled = result.status == MAI_AVATAR_CANCELED;
    mai_avatar_result_free(&result);
    if (output == nullptr)
        avatarFailure(env, canceled ? "Avatar upload canceled" : "Avatar upload was not confirmed");
    return output;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass local = env->FindClass("com/kongshang/maichat/AndroidFileOperations");
    if (local == nullptr) return JNI_ERR;
    sFileOperationsClass = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (sFileOperationsClass == nullptr) return JNI_ERR;
    sPublishNewFileMethod = env->GetStaticMethodID(sFileOperationsClass, "publishNewFile", "([B[B)I");
    if (sPublishNewFileMethod == nullptr) {
        env->DeleteGlobalRef(sFileOperationsClass);
        sFileOperationsClass = nullptr;
        return JNI_ERR;
    }
    sFileJavaVm = vm;
    maiSetAndroidPublishNewFileCallback(publishNewFileInJava);
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeCreate(JNIEnv *env,
                                                              jclass ownerClass) {
  if (sAssistantClass == nullptr) {
    sAssistantClass = reinterpret_cast<jclass>(env->NewGlobalRef(ownerClass));
    sAssistantChangedMethod = env->GetStaticMethodID(ownerClass,
        "onNativeSessionChanged", "(Ljava/lang/String;)V");
  }
  void* agent = maiMobileAgentCreate();
  if (agent != nullptr) maiMobileAgentSetChangeHandler(agent, publishAgentChangeToJava);
  return reinterpret_cast<jlong>(agent);
}
extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeDestroy(JNIEnv *, jclass,
                                                               jlong handle) {
  maiMobileAgentDestroy(reinterpret_cast<void *>(handle));
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeSetHostToolHandler(
    JNIEnv *env, jclass, jlong handle, jobject owner) {
  if (handle == 0 || owner == nullptr)
    return JNI_FALSE;
  auto *context = new JniHostToolContext();
  if (env->GetJavaVM(&context->vm) != JNI_OK) {
    delete context;
    return JNI_FALSE;
  }
  context->owner = env->NewGlobalRef(owner);
  jclass ownerClass = env->GetObjectClass(owner);
  context->invoke =
      env->GetMethodID(ownerClass, "onNativeHostTool", "([B[B)[B");
  env->DeleteLocalRef(ownerClass);
  if (context->owner == nullptr || context->invoke == nullptr ||
      env->ExceptionCheck()) {
    env->ExceptionClear();
    if (context->owner != nullptr)
      env->DeleteGlobalRef(context->owner);
    delete context;
    return JNI_FALSE;
  }
  if (maiMobileAgentSetHostToolHandler(
          reinterpret_cast<void *>(handle), context, hostToolCall,
          hostToolResponseFree, hostToolContextRelease) == 0) {
    env->DeleteGlobalRef(context->owner);
    delete context;
    return JNI_FALSE;
  }
  return JNI_TRUE;
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeRequest(
    JNIEnv *env, jclass, jlong handle, jbyteArray data) {
  if (!data)
    return nullptr;
  jsize size = env->GetArrayLength(data);
  std::string request(static_cast<size_t>(size), '\0');
  // 非空字符串的首元素可写；旧标准库的 data() 仍可能返回 const char*。
  if (size > 0)
    env->GetByteArrayRegion(data, 0, size,
                            reinterpret_cast<jbyte *>(&request[0]));
  if (env->ExceptionCheck())
    return nullptr;
  char *response =
      maiMobileAgentRequest(reinterpret_cast<void *>(handle), request.c_str());
  if (!response)
    return nullptr;
  jsize count = static_cast<jsize>(std::strlen(response));
  jbyteArray result = env->NewByteArray(count);
  if (result)
    env->SetByteArrayRegion(result, 0, count,
                            reinterpret_cast<const jbyte *>(response));
  maiMobileAgentFree(response);
  return result;
}

// 即时撤销/SDK 派发核准不进入 Agent 串行队列，避免 Main/SDK 回调递归等待自身。
// 使用标准 UTF-8 bytes 而非 JNI modified UTF-8，晚到回调也不保存 raw native handle。
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeSessionIMControl(
    JNIEnv* env, jclass, jbyteArray data) {
  if (data == nullptr) return nullptr;
  const jsize size = env->GetArrayLength(data);
  if (size <= 0 || size > 256 * 1024) return nullptr;
  std::string request(static_cast<std::size_t>(size), '\0');
  env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(&request[0]));
  if (env->ExceptionCheck()) return nullptr;
  if (request.find('\0') != std::string::npos) return nullptr;
  char* response = maiMobileSessionIMControl(request.c_str());
  if (response == nullptr) return nullptr;
  const auto count = static_cast<jsize>(std::strlen(response));
  jbyteArray result = env->NewByteArray(count);
  if (result != nullptr)
    env->SetByteArrayRegion(result, 0, count, reinterpret_cast<const jbyte*>(response));
  maiMobileAgentFree(response);
  return result;
}

// 与控制入口区分：此函数会读本地 SQLite，Java 只能在普通准备 I/O worker 调用。
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeIMHistorySnapshot(
    JNIEnv* env, jclass, jbyteArray data) {
  if (data == nullptr) return nullptr;
  const jsize size = env->GetArrayLength(data);
  if (size <= 0 || size > 1024 * 1024) return nullptr;
  std::string request(static_cast<std::size_t>(size), '\0');
  env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(&request[0]));
  if (env->ExceptionCheck() || request.find('\0') != std::string::npos) return nullptr;
  char* response = maiMobileIMHistorySnapshot(request.c_str());
  if (response == nullptr) return nullptr;
  const auto count = static_cast<jsize>(std::strlen(response));
  jbyteArray result = env->NewByteArray(count);
  if (result != nullptr)
    env->SetByteArrayRegion(result, 0, count, reinterpret_cast<const jbyte*>(response));
  maiMobileAgentFree(response);
  return result;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeMatteVideo(
    JNIEnv *env, jclass, jbyteArray arguments, jbyteArray workspace,
    jbyteArray modelPath, jbyteArray runtimePath) {
  if (arguments == nullptr || workspace == nullptr || modelPath == nullptr ||
      runtimePath == nullptr)
    return nullptr;
  const auto read = [env](jbyteArray value) {
    std::string output(static_cast<std::size_t>(env->GetArrayLength(value)), '\0');
    if (!output.empty())
      env->GetByteArrayRegion(value, 0, static_cast<jsize>(output.size()),
                              reinterpret_cast<jbyte *>(&output[0]));
    return output;
  };
  const std::string args = read(arguments);
  const std::string root = read(workspace);
  const std::string model = read(modelPath);
  const std::string runtime = read(runtimePath);
  if (env->ExceptionCheck()) return nullptr;
  char *response = maiMobileMatteVideo(args.c_str(), root.c_str(), model.c_str(),
                                        runtime.c_str(), nullptr);
  if (response == nullptr) return nullptr;
  const jsize size = static_cast<jsize>(std::strlen(response));
  jbyteArray bytes = env->NewByteArray(size);
  if (bytes != nullptr)
    env->SetByteArrayRegion(bytes, 0, size,
                            reinterpret_cast<const jbyte *>(response));
  maiMobileAgentFree(response);
  return bytes;
}

#ifndef NDEBUG
// Debug instrumentation calls the same download tool with an App-private workspace and local HTTP fixture.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_DownloadPublishInstrumentedTest_nativeDownload(
    JNIEnv *env, jclass, jbyteArray arguments, jbyteArray workspace) {
  if (arguments == nullptr || workspace == nullptr) return nullptr;
  const auto read = [env](jbyteArray value) {
    const jsize size = env->GetArrayLength(value);
    std::string result(static_cast<std::size_t>(size), '\0');
    if (size > 0)
      env->GetByteArrayRegion(value, 0, size,
                              reinterpret_cast<jbyte *>(&result[0]));
    return result;
  };
  MaiToolContext context;
  context.root = read(workspace);
  const MaiToolResult result = makeMaiDownloadFileTool()->execute(read(arguments), context);
  const std::string response = result.hasError()
      ? "ERR\n" + result.error().message() : "OK\n" + result.output();
  jbyteArray bytes = env->NewByteArray(static_cast<jsize>(response.size()));
  if (bytes != nullptr)
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(response.size()),
                            reinterpret_cast<const jbyte *>(response.data()));
  return bytes;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_kongshang_maichat_DownloadPublishInstrumentedTest_nativePublishNewFile(
    JNIEnv *env, jclass, jbyteArray source, jbyteArray destination) {
  if (source == nullptr || destination == nullptr) return JNI_FALSE;
  const auto read = [env](jbyteArray value) {
    const jsize size = env->GetArrayLength(value);
    std::string result(static_cast<std::size_t>(size), '\0');
    if (size > 0)
      env->GetByteArrayRegion(value, 0, size,
                              reinterpret_cast<jbyte *>(&result[0]));
    return result;
  };
  return MaiFileSystem::publishNewFile(MaiFilePath::fromUtf8(read(source)),
                                        MaiFilePath::fromUtf8(read(destination)))
      ? JNI_FALSE : JNI_TRUE;
}
#endif

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeFilterBitmap(
    JNIEnv *env, jclass, jobject bitmap, jstring operation,
    jintArray dimensions, jobjectArray portraitMetadata, jbyteArray profileBytes, jbyteArray sourceBytes) {
  if (bitmap == nullptr || operation == nullptr || dimensions == nullptr ||
      env->GetArrayLength(dimensions) < 2)
    return nullptr;
  AndroidBitmapInfo info = {};
  if (AndroidBitmap_getInfo(env, bitmap, &info) !=
          ANDROID_BITMAP_RESULT_SUCCESS ||
      info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || info.width < 1 ||
      info.height < 1)
    return nullptr;
  const char *spec = env->GetStringUTFChars(operation, nullptr);
  if (spec == nullptr)
    return nullptr;
  void *input = nullptr;
  if (AndroidBitmap_lockPixels(env, bitmap, &input) !=
      ANDROID_BITMAP_RESULT_SUCCESS) {
    env->ReleaseStringUTFChars(operation, spec);
    return nullptr;
  }
  MaiImageFilterResult result{};
  if (profileBytes && sourceBytes) {
    auto read = [env](jbyteArray value) {
      std::string bytes(static_cast<size_t>(env->GetArrayLength(value)), '\0');
      env->GetByteArrayRegion(value, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<jbyte*>(bytes.data()));
      return bytes;
    };
    const auto profile = read(profileBytes), source = read(sourceBytes);
    const auto edited = maiPortraitProcessRgba(static_cast<const unsigned char *>(input),
        static_cast<int>(info.width), static_cast<int>(info.height), static_cast<int>(info.stride),
        spec, profile.c_str(), source.c_str());
    result = {edited.rgba, edited.width, edited.height, edited.error};
    if (edited.metadata) {
      if (portraitMetadata && env->GetArrayLength(portraitMetadata) > 0) {
        const auto size = static_cast<jsize>(std::strlen(edited.metadata));
        jbyteArray metadata = env->NewByteArray(size);
        env->SetByteArrayRegion(metadata, 0, size, reinterpret_cast<const jbyte*>(edited.metadata));
        env->SetObjectArrayElement(portraitMetadata, 0, metadata);
        env->DeleteLocalRef(metadata);
      }
      maiPortraitFree(edited.metadata);
    }
  } else {
    result = maiImageFilterRgba(static_cast<const unsigned char *>(input), static_cast<int>(info.width),
                               static_cast<int>(info.height), static_cast<int>(info.stride), spec);
  }
  AndroidBitmap_unlockPixels(env, bitmap);
  env->ReleaseStringUTFChars(operation, spec);
  if (result.error != nullptr) {
    jclass type = env->FindClass("java/lang/IllegalArgumentException");
    if (type != nullptr)
      env->ThrowNew(type, result.error);
    maiImageFilterFree(result.error);
    return nullptr;
  }
  if (result.rgba == nullptr)
    return nullptr;
  const jint outputDimensions[2] = {result.width, result.height};
  env->SetIntArrayRegion(dimensions, 0, 2, outputDimensions);
  const jsize outputSize = static_cast<jsize>(
      static_cast<long long>(result.width) * result.height * 4);
  jbyteArray output = env->NewByteArray(outputSize);
  if (output != nullptr)
    env->SetByteArrayRegion(output, 0, outputSize,
                            reinterpret_cast<const jbyte *>(result.rgba));
  maiImageFilterFree(result.rgba);
  return output;
}

namespace {
std::string portraitUtf8(JNIEnv* env, jbyteArray input) {
  if (!input) return {};
  std::string result(static_cast<size_t>(env->GetArrayLength(input)), '\0');
  env->GetByteArrayRegion(input, 0, static_cast<jsize>(result.size()), reinterpret_cast<jbyte*>(result.data()));
  return result;
}
}
extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeConfigurePortrait(JNIEnv* env,jclass,jbyteArray directory,jbyteArray runtime) {
  const auto models=portraitUtf8(env,directory),library=portraitUtf8(env,runtime);
  maiPortraitConfigure(models.c_str(),library.c_str(),nullptr);
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativePortraitRequest(JNIEnv* env,jclass,jbyteArray directory,jbyteArray request,jbyteArray output,jint action) {
  const auto profile=portraitUtf8(env,directory),arguments=portraitUtf8(env,request),path=portraitUtf8(env,output);
  char* response=nullptr;
  if(action==1)response=maiPortraitOriginalSource(profile.c_str(),arguments.c_str());
  else if(action==2)response=maiPortraitCommitResult(profile.c_str(),arguments.c_str(),path.c_str());
  else response=maiPortraitPersonalizationRequest(profile.c_str(),arguments.c_str());
  if(!response)return nullptr;
  const auto size=static_cast<jsize>(std::strlen(response));
  jbyteArray result=env->NewByteArray(size);
  if(result)env->SetByteArrayRegion(result,0,size,reinterpret_cast<const jbyte*>(response));
  maiPortraitFree(response);
  return result;
}

// Android owns one full-sized mutable sRGB destination and an L8 diagnostic mask.
// These entries borrow bounded original samples; the shared core owns skin/subject policy.
namespace {
struct PortraitBitmapLock {
    JNIEnv* env;
    jobject bitmap;
    AndroidBitmapInfo info{};
    void* pixels = nullptr;
    bool premultiplied = true;
    bool mutablePixels = false;
    PortraitBitmapLock(JNIEnv* environment, jobject image) : env(environment), bitmap(image) {
        if (!bitmap || AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
            info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || info.width == 0 || info.height == 0 ||
            static_cast<uint64_t>(info.width) * info.height > 32'000'000 ||
            info.stride < static_cast<uint64_t>(info.width) * 4)
            return;
        jclass type = env->GetObjectClass(bitmap);
        jmethodID method = type ? env->GetMethodID(type, "isPremultiplied", "()Z") : nullptr;
        if (method) premultiplied = env->CallBooleanMethod(bitmap, method) == JNI_TRUE;
        jmethodID mutableMethod = type ? env->GetMethodID(type, "isMutable", "()Z") : nullptr;
        if (mutableMethod) mutablePixels = env->CallBooleanMethod(bitmap, mutableMethod) == JNI_TRUE;
        if (type) env->DeleteLocalRef(type);
        if (env->ExceptionCheck()) return;
        if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) pixels = nullptr;
    }
    ~PortraitBitmapLock() {
        if (pixels) AndroidBitmap_unlockPixels(env, bitmap);
    }
};

struct PortraitRegionOwner {
    MaiPortraitRegionResult value{};
    ~PortraitRegionOwner() {
        maiPortraitFree(value.rgba);
        maiPortraitFree(value.mask);
        maiPortraitFree(value.metadata);
        maiPortraitFree(value.error);
    }
};

jbyteArray portraitBytes(JNIEnv* env, const char* value) {
    if (!value) return nullptr;
    const auto size = static_cast<jsize>(std::strlen(value));
    jbyteArray result = env->NewByteArray(size);
    if (result) env->SetByteArrayRegion(result, 0, size, reinterpret_cast<const jbyte*>(value));
    return result;
}
jbyteArray ownedPortraitBytes(JNIEnv* env, char* value) {
    std::unique_ptr<char, decltype(&maiPortraitFree)> owned(value, &maiPortraitFree);
    return portraitBytes(env, value);
}
jbyteArray portraitHostFailure(JNIEnv* env) {
    return portraitBytes(
        env,
        R"({"code":"invalid_host_pixels","stage":"android_region","message":"Portrait pixels or region bounds are invalid","hint":"Retry with a supported source and enough device memory"})");
}

// Never reinterpret premultiplied PNG memory as straight color. Opaque photo regions avoid
// this copy; translucent regions get one bounded ROI copy, then are premultiplied on writeback.
const unsigned char* straightPortraitPixels(const unsigned char* input, int width, int height, int stride,
                                            bool premultiplied, std::vector<unsigned char>& straight) {
    if (!premultiplied) return input;
    bool translucent = false;
    for (int y = 0; y < height && !translucent; ++y)
        for (int x = 0; x < width; ++x)
            if (input[static_cast<size_t>(y) * stride + x * 4 + 3] != 255) {
                translucent = true;
                break;
            }
    if (!translucent) return input;
    straight.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto* pixel = input + static_cast<size_t>(y) * stride + x * 4;
            auto* output = straight.data() + (static_cast<size_t>(y) * width + x) * 4;
            const unsigned alpha = pixel[3];
            for (int channel = 0; channel < 3; ++channel)
                output[channel] = alpha ? static_cast<unsigned char>(std::min(
                                              255U, (static_cast<unsigned>(pixel[channel]) * 255 + alpha / 2) / alpha))
                                        : 0;
            output[3] = static_cast<unsigned char>(alpha);
        }
    return straight.data();
}

void writePortraitPixels(unsigned char* target, int stride, bool premultiplied, const unsigned char* rgba,
                         int sourceStride, int width, int height) {
    for (int row = 0; row < height; ++row) {
        auto* output = target + static_cast<size_t>(row) * stride;
        const auto* pixels = rgba + static_cast<size_t>(row) * sourceStride;
        if (!premultiplied) {
            std::memcpy(output, pixels, static_cast<size_t>(width) * 4);
        } else {
            for (int column = 0; column < width; ++column) {
                const auto* pixel = pixels + column * 4;
                for (int channel = 0; channel < 3; ++channel)
                    output[column * 4 + channel] = static_cast<unsigned char>((pixel[channel] * pixel[3] + 127) / 255);
                output[column * 4 + 3] = pixel[3];
            }
        }
    }
}

MaiPortraitSkinContext* skinContext(jlong handle) {
    return reinterpret_cast<MaiPortraitSkinContext*>(static_cast<intptr_t>(handle));
}
}  // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_MaiPortraitImage_nativeValidate(JNIEnv* env, jclass, jbyteArray arguments) {
    try {
        const auto encoded = portraitUtf8(env, arguments);
        return ownedPortraitBytes(env, maiPortraitValidateArguments("beautify", encoded.c_str(), 1));
    } catch (...) {
        return portraitHostFailure(env);
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_kongshang_maichat_MaiPortraitImage_nativeMergeFaces(JNIEnv* env,
                                                                                                     jclass,
                                                                                                     jbyteArray hints) {
    try {
        const auto encoded = portraitUtf8(env, hints);
        return ownedPortraitBytes(env, maiPortraitMergeFaceHints(encoded.c_str()));
    } catch (...) {
        return portraitHostFailure(env);
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_kongshang_maichat_MaiPortraitImage_nativeAnalyze(
    JNIEnv* env, jclass, jobject proxy, jobject global, jint width, jint height, jbyteArray hints,
    jbyteArray arguments, jbyteArray profile, jbyteArray original, jbyteArray fingerprint, jlongArray handle) {
    try {
        PortraitBitmapLock image(env, proxy), features(env, global);
        if (!image.pixels || image.info.width > 512 || image.info.height > 512 || !features.pixels ||
            features.info.width != 64 || features.info.height != 64 || !handle || env->GetArrayLength(handle) != 1)
            return portraitHostFailure(env);
        std::vector<unsigned char> straight, globalStraight;
        const auto* input = straightPortraitPixels(
            static_cast<const unsigned char*>(image.pixels), static_cast<int>(image.info.width),
            static_cast<int>(image.info.height), static_cast<int>(image.info.stride), image.premultiplied, straight);
        const auto* globalInput = straightPortraitPixels(static_cast<const unsigned char*>(features.pixels), 64, 64,
            static_cast<int>(features.info.stride), features.premultiplied, globalStraight);
        const auto encodedHints = portraitUtf8(env, hints), encoded = portraitUtf8(env, arguments),
                   directory = portraitUtf8(env, profile), path = portraitUtf8(env, original),
                   identity = portraitUtf8(env, fingerprint);
        const auto analysis = maiPortraitAnalyzeSkinRgba(input, static_cast<int>(image.info.width),
            static_cast<int>(image.info.height),
            straight.empty() ? static_cast<int>(image.info.stride) : static_cast<int>(image.info.width) * 4,
            width, height, encodedHints.c_str(), encoded.c_str(), directory.c_str(), path.c_str(), identity.c_str(),
            globalInput, globalStraight.empty() ? static_cast<int>(features.info.stride) : 256);
        std::unique_ptr<MaiPortraitSkinContext, decltype(&maiPortraitFreeSkinContext)> context(
            analysis.context, &maiPortraitFreeSkinContext);
        std::unique_ptr<char, decltype(&maiPortraitFree)> metadata(analysis.metadata, &maiPortraitFree),
            error(analysis.error, &maiPortraitFree);
        if (error) return portraitBytes(env, error.get());
        if (!context || !metadata) return portraitHostFailure(env);
        jbyteArray response = portraitBytes(env, metadata.get());
        if (!response || env->ExceptionCheck()) return nullptr;
        const jlong value = static_cast<jlong>(reinterpret_cast<intptr_t>(context.get()));
        env->SetLongArrayRegion(handle, 0, 1, &value);
        if (env->ExceptionCheck()) return nullptr;
        context.release();
        return response;
    } catch (...) {
        return portraitHostFailure(env);
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_kongshang_maichat_MaiPortraitImage_nativeRefine(
    JNIEnv* env, jclass, jlong context, jobject proxy) {
    try {
        PortraitBitmapLock image(env, proxy);
        // 仅原图人物细化工作面允许 1024；全图 Analyze 的 512 预算不变。
        if (!image.pixels || image.info.width > 1024 || image.info.height > 1024) return portraitHostFailure(env);
        std::vector<unsigned char> straight;
        const auto* input = straightPortraitPixels(static_cast<const unsigned char*>(image.pixels),
            static_cast<int>(image.info.width), static_cast<int>(image.info.height),
            static_cast<int>(image.info.stride), image.premultiplied, straight);
        return ownedPortraitBytes(env, maiPortraitRefineSkinRegionRgba(skinContext(context), input,
            static_cast<int>(image.info.width), static_cast<int>(image.info.height),
            straight.empty() ? static_cast<int>(image.info.stride) : static_cast<int>(image.info.width) * 4));
    } catch (...) { return portraitHostFailure(env); }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_kongshang_maichat_MaiPortraitImage_nativeProtect(
    JNIEnv* env, jclass, jlong context, jobject face) {
    try {
        if (!face) return ownedPortraitBytes(env, maiPortraitProtectSkinFaceRegion(skinContext(context), nullptr, 0, 0, 0));
        PortraitBitmapLock image(env, face);
        if (!image.pixels || image.info.width > 2048 || image.info.height > 2048) return portraitHostFailure(env);
        std::vector<unsigned char> straight;
        const auto* input = straightPortraitPixels(static_cast<const unsigned char*>(image.pixels),
            static_cast<int>(image.info.width), static_cast<int>(image.info.height),
            static_cast<int>(image.info.stride), image.premultiplied, straight);
        return ownedPortraitBytes(env, maiPortraitProtectSkinFaceRegion(skinContext(context), input,
            static_cast<int>(image.info.width), static_cast<int>(image.info.height),
            straight.empty() ? static_cast<int>(image.info.stride) : static_cast<int>(image.info.width) * 4));
    } catch (...) { return portraitHostFailure(env); }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_kongshang_maichat_MaiPortraitImage_nativeTile(
    JNIEnv* env, jclass, jlong context, jint index, jobject sample, jobject destination, jbyteArray mask,
    jint sampleX, jint sampleY, jint writeX, jint writeY, jint width, jint height) {
    try {
        PortraitBitmapLock image(env, sample), output(env, destination);
        if (!image.pixels || !output.pixels || !output.mutablePixels || !mask || sampleX < 0 || sampleY < 0 ||
            width < 1 || height < 1 || width > 1024 || height > 1024 || image.info.width > 1040 ||
            image.info.height > 1040 || writeX < sampleX || writeY < sampleY ||
            static_cast<int64_t>(sampleX) + image.info.width > output.info.width ||
            static_cast<int64_t>(sampleY) + image.info.height > output.info.height ||
            static_cast<int64_t>(writeX) + width > static_cast<int64_t>(sampleX) + image.info.width ||
            static_cast<int64_t>(writeY) + height > static_cast<int64_t>(sampleY) + image.info.height ||
            static_cast<int64_t>(env->GetArrayLength(mask)) != static_cast<int64_t>(output.info.width) * output.info.height)
            return portraitHostFailure(env);
        std::vector<unsigned char> straight;
        const auto* input = straightPortraitPixels(static_cast<const unsigned char*>(image.pixels),
            static_cast<int>(image.info.width), static_cast<int>(image.info.height),
            static_cast<int>(image.info.stride), image.premultiplied, straight);
        PortraitRegionOwner edited;
        edited.value = maiPortraitProcessSkinTile(skinContext(context), index, input, static_cast<int>(image.info.width),
            static_cast<int>(image.info.height),
            straight.empty() ? static_cast<int>(image.info.stride) : static_cast<int>(image.info.width) * 4);
        if (edited.value.error) return portraitBytes(env, edited.value.error);
        if (!edited.value.rgba || !edited.value.mask || edited.value.width != static_cast<int>(image.info.width) ||
            edited.value.height != static_cast<int>(image.info.height)) return portraitHostFailure(env);
        const size_t offset = static_cast<size_t>(writeY - sampleY) * image.info.width + writeX - sampleX;
        auto* target = static_cast<unsigned char*>(output.pixels) + static_cast<size_t>(writeY) * output.info.stride + writeX * 4;
        writePortraitPixels(target, static_cast<int>(output.info.stride), output.premultiplied,
            edited.value.rgba + offset * 4, edited.value.width * 4, width, height);
        for (int row = 0; row < height; ++row) {
            const auto* alpha = edited.value.mask + offset + static_cast<size_t>(row) * image.info.width;
            env->SetByteArrayRegion(mask, static_cast<jsize>(static_cast<int64_t>(writeY + row) * output.info.width + writeX),
                width, reinterpret_cast<const jbyte*>(alpha));
            if (env->ExceptionCheck()) return nullptr;
        }
        return portraitBytes(env, R"({"ok":true,"output":{}})");
    } catch (...) { return portraitHostFailure(env); }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_kongshang_maichat_MaiPortraitImage_nativeFinish(
    JNIEnv* env, jclass, jlong context) {
    return ownedPortraitBytes(env, maiPortraitFinishSkin(skinContext(context)));
}
extern "C" JNIEXPORT void JNICALL Java_com_kongshang_maichat_MaiPortraitImage_nativeFree(
    JNIEnv*, jclass, jlong context) {
    maiPortraitFreeSkinContext(skinContext(context));
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_kongshang_maichat_MaiPortraitImage_nativeApply(
    JNIEnv* env, jclass, jobject source, jint x, jint y, jint width, jint height, jbyteArray arguments,
    jbyteArray profile, jbyteArray original, jbyteArray context) {
    try {
        PortraitBitmapLock image(env, source);
        if (!image.pixels || !image.mutablePixels || x < 0 || y < 0 || width < 1 || height < 1 || width > 2048 ||
            height > 2048 || static_cast<int64_t>(x) + width > image.info.width ||
            static_cast<int64_t>(y) + height > image.info.height)
            return portraitHostFailure(env);
        auto* target = static_cast<unsigned char*>(image.pixels) + static_cast<size_t>(y) * image.info.stride + x * 4;
        std::vector<unsigned char> straight;
        const auto* input = straightPortraitPixels(target, width, height, static_cast<int>(image.info.stride),
                                                   image.premultiplied, straight);
        const auto specification = portraitUtf8(env, arguments), directory = portraitUtf8(env, profile),
                   path = portraitUtf8(env, original), region = portraitUtf8(env, context);
        PortraitRegionOwner edited;
        edited.value = maiPortraitProcessRegionRgba(
            input, width, height, straight.empty() ? static_cast<int>(image.info.stride) : width * 4,
            specification.c_str(), directory.c_str(), path.c_str(), region.c_str());
        if (edited.value.error) return portraitBytes(env, edited.value.error);
        if (!edited.value.rgba || !edited.value.metadata || edited.value.width != width ||
            edited.value.height != height)
            return portraitHostFailure(env);
        writePortraitPixels(target, static_cast<int>(image.info.stride), image.premultiplied,
            edited.value.rgba, width * 4, width, height);
        // Core output already contains the soft-edge blend. The skin mask is diagnostic only.
        return portraitBytes(env, edited.value.metadata);
    } catch (...) {
        return portraitHostFailure(env);
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_kongshang_maichat_MaiPortraitImage_nativePublish(JNIEnv* env, jclass,
                                                                                                  jbyteArray temporary,
                                                                                                  jbyteArray target) {
    try {
        const auto from = portraitUtf8(env, temporary), to = portraitUtf8(env, target);
        return ownedPortraitBytes(env, maiPortraitPublishOutput(from.c_str(), to.c_str()));
    } catch (...) {
        return portraitHostFailure(env);
    }
}
