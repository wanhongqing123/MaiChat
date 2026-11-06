# 文件作用：独立验证并部署人像模型/ORT，不依赖视频抠图是否启用。
get_filename_component(_portrait_desktop_directory "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MAICHAT_PORTRAIT_MODEL_DIRECTORY "${_portrait_desktop_directory}/../Portrait/models"
  CACHE PATH "Revision-pinned local portrait model directory")
if(WIN32)
  set(_portrait_runtime "${_portrait_desktop_directory}/../../MaiAgent/build/vendor/onnxruntime-windows-x64/onnxruntime.dll")
  set(_portrait_destination "$<TARGET_FILE_DIR:maichat>/PortraitModels")
  set(_portrait_runtime_destination "$<TARGET_FILE_DIR:maichat>/onnxruntime.dll")
  set(_portrait_runtime_directory "$<TARGET_FILE_DIR:maichat>")
else()
  set(_portrait_runtime "${_portrait_desktop_directory}/../../MaiAgent/build/vendor/onnxruntime-macos/libonnxruntime.1.26.0.dylib")
  set(_portrait_destination "$<TARGET_BUNDLE_DIR:maichat>/Contents/Resources/PortraitModels")
  set(_portrait_runtime_destination "$<TARGET_BUNDLE_DIR:maichat>/Contents/Frameworks/libonnxruntime.1.26.0.dylib")
  set(_portrait_runtime_directory "$<TARGET_BUNDLE_DIR:maichat>/Contents/Frameworks")
endif()
set(MAICHAT_PORTRAIT_ORT_RUNTIME "${_portrait_runtime}" CACHE FILEPATH "Bundled portrait ONNX Runtime (target architecture)")
if(NOT EXISTS "${MAICHAT_PORTRAIT_ORT_RUNTIME}")
  message(FATAL_ERROR "Portrait ONNX Runtime is missing: ${MAICHAT_PORTRAIT_ORT_RUNTIME}")
endif()
set(_portrait_models face_landmarks.onnx selfie_multiclass.onnx person_detection.onnx pose_estimation.onnx)
set(_portrait_hashes
  f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c
  35ec1ecd9ee7f85073c99c00020b7f6751b69506eeacf683bc8665f6117f85b0
  47fd5599d6fa17608f03e0eb0ae230baa6e597d7e8a2c8199fe00abea55a701f
  9d89c599319a18fb7d2e28451a883476164543182bafca5f09eb2cf767ed2f3f)
foreach(_index RANGE 0 3)
  list(GET _portrait_models ${_index} _model)
  list(GET _portrait_hashes ${_index} _expected)
  if(NOT EXISTS "${MAICHAT_PORTRAIT_MODEL_DIRECTORY}/${_model}")
    message(FATAL_ERROR "Missing portrait model ${_model}; run scripts/fetch-portrait-models.py")
  endif()
  file(SHA256 "${MAICHAT_PORTRAIT_MODEL_DIRECTORY}/${_model}" _actual)
  if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "Portrait model checksum mismatch: ${_model}")
  endif()
  add_custom_command(TARGET maichat POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_portrait_destination}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${MAICHAT_PORTRAIT_MODEL_DIRECTORY}/${_model}" "${_portrait_destination}/${_model}")
endforeach()
add_custom_command(TARGET maichat POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_portrait_runtime_directory}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MAICHAT_PORTRAIT_ORT_RUNTIME}" "${_portrait_runtime_destination}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MAICHAT_PORTRAIT_MODEL_DIRECTORY}/LICENSE" "${_portrait_destination}/MEDIAPIPE_LICENSE"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MAICHAT_PORTRAIT_MODEL_DIRECTORY}/MODELS.md" "${_portrait_destination}/MODELS.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_portrait_desktop_directory}/../../MaiAgent/third_party/onnxruntime/LICENSE" "${_portrait_destination}/ONNXRUNTIME_LICENSE"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_portrait_desktop_directory}/../../MaiAgent/third_party/onnxruntime/ThirdPartyNotices.txt" "${_portrait_destination}/ONNXRUNTIME_ThirdPartyNotices.txt")
if(WIN32)
  get_filename_component(_portrait_ort_directory "${MAICHAT_PORTRAIT_ORT_RUNTIME}" DIRECTORY)
  if(EXISTS "${_portrait_ort_directory}/onnxruntime_providers_shared.dll")
    add_custom_command(TARGET maichat POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${_portrait_ort_directory}/onnxruntime_providers_shared.dll" "$<TARGET_FILE_DIR:maichat>/onnxruntime_providers_shared.dll")
  endif()
endif()

add_custom_command(TARGET maichat POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${_portrait_desktop_directory}/../Portrait/whitening/LICENSE" "${_portrait_destination}/GPUPIXEL_LICENSE"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${_portrait_desktop_directory}/../Portrait/whitening/NOTICE" "${_portrait_destination}/GPUPIXEL_NOTICE"
  COMMAND ${CMAKE_COMMAND} "-DMODEL_DIRECTORY=${_portrait_destination}"
    "-DRUNTIME_PATH=${_portrait_runtime_destination}"
    -P "${_portrait_desktop_directory}/cmake/VerifyPortraitAssets.cmake")
