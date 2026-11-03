# 文件作用：从固定的本地 llama.cpp/ggml/mtmd 源码构建可复用静态库，不下载 SDK 或模型。
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

  set(BUILD_SHARED_LIBS OFF)
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
  set(GGML_BACKEND_DL OFF)
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
  add_subdirectory("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/llama.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/llama" EXCLUDE_FROM_ALL)

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
  target_link_libraries(mai_llama INTERFACE mtmd llama)
  target_compile_features(mai_llama INTERFACE cxx_std_17)
  target_compile_definitions(mai_llama INTERFACE
    MAI_LLAMA_SOURCE_COMMIT="ee17f293b638295651c6ff4752a4dce102c6b8e4")
endfunction()
