# 文件作用：移动端和 Qt 桌面共用唯一的人像核心目标；宿主负责模型与 ORT 打包。
cmake_minimum_required(VERSION 3.21)
add_library(maichat_portrait STATIC
  MaiPortraitEngine.cpp MaiPortraitRegion.cpp
  MaiPortraitPersonalization.cpp MaiPortraitBridge.cpp
  MaiBodySkinRetouch.cpp MaiPortraitWhiteningLut.cpp)
set_target_properties(maichat_portrait PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(maichat_portrait PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/nlohmann"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/sqlite"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/onnxruntime/include/onnxruntime/core/session"
  "${CMAKE_BINARY_DIR}" "${CMAKE_BINARY_DIR}/opencv"
  ${OPENCV_CONFIG_FILE_INCLUDE_DIR} ${OpenCV_BINARY_DIR}
  "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/opencv/modules/core/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/opencv/modules/imgproc/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/opencv/modules/photo/include"
  ${MAI_OPENSSL_INCLUDE_DIR} ${OPENSSL_INCLUDE_DIR}
  "${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/openssl/include")
target_compile_features(maichat_portrait PUBLIC cxx_std_17)
target_link_libraries(maichat_portrait PUBLIC maiagent mai_sqlite opencv_core opencv_imgproc opencv_photo)
if(MAI_OPENSSL_CRYPTO_TARGET)
  target_link_libraries(maichat_portrait PUBLIC ${MAI_OPENSSL_CRYPTO_TARGET})
else()
  target_link_libraries(maichat_portrait PUBLIC ${OPENSSL_CRYPTO_LIBRARY})
endif()
if(CMAKE_DL_LIBS)
  target_link_libraries(maichat_portrait PRIVATE ${CMAKE_DL_LIBS})
endif()
