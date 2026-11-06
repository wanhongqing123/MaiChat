# 文件作用：构建后校验真正进入应用包的四个模型，防止只验证源码资产。
if(NOT DEFINED MODEL_DIRECTORY OR NOT DEFINED RUNTIME_PATH OR NOT EXISTS "${RUNTIME_PATH}")
  message(FATAL_ERROR "Packaged portrait ONNX Runtime is missing")
endif()
set(_names face_landmarks.onnx selfie_multiclass.onnx person_detection.onnx pose_estimation.onnx)
set(_hashes
  f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c
  35ec1ecd9ee7f85073c99c00020b7f6751b69506eeacf683bc8665f6117f85b0
  47fd5599d6fa17608f03e0eb0ae230baa6e597d7e8a2c8199fe00abea55a701f
  9d89c599319a18fb7d2e28451a883476164543182bafca5f09eb2cf767ed2f3f)
foreach(_index RANGE 0 3)
  list(GET _names ${_index} _name)
  list(GET _hashes ${_index} _expected)
  if(NOT EXISTS "${MODEL_DIRECTORY}/${_name}")
    message(FATAL_ERROR "Packaged portrait model is missing: ${_name}")
  endif()
  file(SHA256 "${MODEL_DIRECTORY}/${_name}" _actual)
  if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "Packaged portrait model checksum mismatch: ${_name}")
  endif()
endforeach()
message(STATUS "Packaged portrait models verified (4/4), ONNX Runtime present")
