# 文件作用：从固定的本地 llama.cpp/ggml/mtmd 源码构建可复用库，不下载 SDK 或模型。
# Android arm64 使用官方 CPU 动态变体按实际指令集分派，其余平台保留静态依赖。
# 供应商 CLI 的进程状态尚未审计，因此只构建库；视频文件抽帧复用宿主已有 FFmpeg。
include_guard(GLOBAL)

function(mai_add_llama_dependencies)
  if(TARGET mai_llama)
    return()
  endif()
  if(TARGET llama OR TARGET mtmd OR TARGET ggml)
    message(FATAL_ERROR "mai_llama requires its pinned local llama.cpp and ggml targets")
  endif()

  option(MAI_LLAMA_METAL "Build the embedded Metal backend on Apple platforms" ${APPLE})
  if(MAI_LLAMA_METAL AND NOT APPLE)
    message(FATAL_ERROR "MAI_LLAMA_METAL requires an Apple target")
  endif()

  set(mai_llama_android_cpu_variants OFF)
  if(ANDROID AND (ANDROID_ABI STREQUAL "arm64-v8a" OR
      CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a"))
    set(mai_llama_android_cpu_variants ON)
  endif()
  # 此函数作用域只改变 llama/ggml/mtmd，不把其它依赖或 Windows 发布改成共享构建。
  # 高级指令只进入官方 CPU MODULE；JNI 与特征检测继续使用 ARMv8.0 基线。
  set(BUILD_SHARED_LIBS ${mai_llama_android_cpu_variants})
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_AUTOMOC OFF)
  set(CMAKE_AUTOUIC OFF)
  set(CMAKE_AUTORCC OFF)
  set(LLAMA_BUILD_COMMON OFF)
  set(LLAMA_BUILD_TOOLS OFF)
  set(LLAMA_BUILD_EXAMPLES OFF)
  set(LLAMA_BUILD_TESTS OFF)
  set(LLAMA_BUILD_SERVER OFF)
  set(LLAMA_BUILD_APP OFF)
  set(LLAMA_BUILD_UI OFF)
  set(LLAMA_CONNECT OFF)
  set(LLAMA_BUILD_MTMD ON)
  set(LLAMA_USE_SYSTEM_GGML OFF)
  set(LLAMA_OPENSSL OFF)
  set(LLAMA_SUBPROCESS OFF)
  # This flag controls upstream's external ffmpeg/ffprobe adapter, not the host's
  # in-process FFmpeg or mtmd's ability to consume already-decoded video frames.
  set(MTMD_VIDEO OFF)
  set(GGML_BUILD_TESTS OFF)
  set(GGML_BUILD_EXAMPLES OFF)
  set(GGML_BACKEND_DL ${mai_llama_android_cpu_variants})
  set(GGML_CPU_ALL_VARIANTS ${mai_llama_android_cpu_variants})
  if(mai_llama_android_cpu_variants)
    # 单一 -march 会抬高所有手机的最低指令集，不能与运行时变体分派混用。
    set(GGML_CPU_ARM_ARCH "")
  endif()
  set(GGML_NATIVE OFF)
  set(GGML_OPENMP OFF)
  set(GGML_CCACHE OFF)
  if(ANDROID AND (ANDROID_ABI STREQUAL "armeabi-v7a" OR
      CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a"))
    # The upstream llamafile SGEMM uses FP16 NEON loads unavailable on ARMv7.
    # Keep the regular ggml ARM/NEON CPU kernels and the existing 32-bit ABI.
    set(GGML_LLAMAFILE OFF)
    message(STATUS "Android armeabi-v7a: using regular ggml CPU kernels without llamafile")
  endif()
  set(GGML_METAL ${MAI_LLAMA_METAL})
  set(GGML_METAL_EMBED_LIBRARY ${MAI_LLAMA_METAL})
  set(GGML_BLAS ${APPLE})
  if(APPLE)
    set(GGML_BLAS_VENDOR Apple)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
      set(GGML_METAL_TARGET_OS ios)
    else()
      set(GGML_METAL_TARGET_OS macos)
    endif()
  endif()

  # A vendored directory has no .git. Do not mislabel the engine with MaiChat's
  # enclosing repository commit when upstream probes build information.
  set(LLAMA_BUILD_NUMBER 12065)
  set(LLAMA_BUILD_COMMIT ee17f293b638295651c6ff4752a4dce102c6b8e4)
  set(GGML_BUILD_COMMIT ${LLAMA_BUILD_COMMIT})
  set(GGML_GIT_DIRTY 0)
  set(GIT_EXE "")
  # AGP 从调用方的 obj/<abi> 收集 native 库；上游会把所有库重定向到自己的 bin。
  set(mai_llama_parent_library_output_directory "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
  add_subdirectory("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/llama.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/llama" EXCLUDE_FROM_ALL)
  if(mai_llama_android_cpu_variants)
    # APK 的 nativeLibraryDir 按未版本化 .so 分发；不能依赖安装目录中的 SONAME 符号链接。
    foreach(target ggml-base ggml llama mtmd)
      set_target_properties(${target} PROPERTIES VERSION "" SOVERSION "")
    endforeach()
    if(IS_ABSOLUTE "${mai_llama_parent_library_output_directory}")
      # MODULE 不在 JNI 的链接依赖列表中，必须与共享依赖同处 AGP 的库收集目录。
      # 仍由 ggml 的 add_dependencies 构建全部变体，不把高级 ISA 硬链接进 JNI。
      set_target_properties(ggml-base ggml llama mtmd
        ggml-cpu-android_armv8.0_1 ggml-cpu-android_armv8.2_1
        ggml-cpu-android_armv8.2_2 ggml-cpu-android_armv8.6_1
        ggml-cpu-android_armv9.0_1 ggml-cpu-android_armv9.2_1
        ggml-cpu-android_armv9.2_2 PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${mai_llama_parent_library_output_directory}")
    endif()
  endif()

  # 主模型仅复用固定版本的模板、Jinja、PEG 与 JSON Schema 语法，不引入上游 CLI、
  # 下载器、HTTP server、common.cpp 文件操作或 common 日志线程。保留 LLAMA_BUILD_COMMON=OFF，
  # 因此 mtmd 仍不链接 common。辅助实现仅保留上游纯字符串/tokenizer 语义并屏蔽敏感日志。
  set(mai_llama_common "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/llama.cpp/common")
  include("${mai_llama_common}/parsers/sources.cmake")
  add_library(mai_llama_chat STATIC
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../Agent/MaiLlamaChatSupport.cpp"
    ${LLAMA_CHAT_PARSERS_SOURCES})
  foreach(source chat.cpp chat-auto-parser-generator.cpp chat-auto-parser-helpers.cpp
      chat-diff-analyzer.cpp chat-peg-parser.cpp peg-parser.cpp json.cpp json-schema.cpp
      json-schema-to-grammar.cpp trie.cpp unicode.cpp jinja/lexer.cpp jinja/parser.cpp
      jinja/runtime.cpp jinja/value.cpp jinja/string.cpp jinja/caps.cpp)
    target_sources(mai_llama_chat PRIVATE "${mai_llama_common}/${source}")
  endforeach()
  target_include_directories(mai_llama_chat PUBLIC "${mai_llama_common}"
    PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include")
  target_link_libraries(mai_llama_chat PUBLIC llama PRIVATE vendor::nlohmann vendor::sheredom)
  target_compile_features(mai_llama_chat PUBLIC cxx_std_17)

  if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
      (NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 16.4))
    # Upstream selects the new ILP64 Accelerate entry points unconditionally; they
    # require iOS 16.4. Its regular CBLAS path also supports the legacy LP64 ABI.
    # -U follows directory -D flags, preserving the product's iOS 16.0 minimum.
    foreach(target ggml-cpu ggml-blas)
      if(TARGET ${target})
        target_compile_options(${target} PRIVATE
          -UACCELERATE_NEW_LAPACK -UACCELERATE_LAPACK_ILP64
          -Werror=unguarded-availability-new)
      endif()
    endforeach()
  endif()

  add_library(mai_llama INTERFACE)
  target_link_libraries(mai_llama INTERFACE mtmd llama mai_llama_chat)
  if(mai_llama_android_cpu_variants)
    target_link_libraries(mai_llama INTERFACE log)
  endif()
  target_compile_features(mai_llama INTERFACE cxx_std_17)
  target_compile_definitions(mai_llama INTERFACE
    MAI_LLAMA_SOURCE_COMMIT="ee17f293b638295651c6ff4752a4dce102c6b8e4")
endfunction()
