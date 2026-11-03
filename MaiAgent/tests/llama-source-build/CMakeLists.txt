cmake_minimum_required(VERSION 3.22)
project(MaiLlamaSourceBuild LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
include("${CMAKE_CURRENT_LIST_DIR}/../../third_party/MaiLlamaDependencies.cmake")
mai_add_llama_dependencies()
add_executable(MaiLlamaBuildSmoke MaiLlamaBuildSmoke.cpp)
target_link_libraries(MaiLlamaBuildSmoke PRIVATE mai_llama)
set_target_properties(MaiLlamaBuildSmoke PROPERTIES MACOSX_BUNDLE OFF)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mai-llama-link-usage.txt"
  CONTENT "pin=ee17f293b638295651c6ff4752a4dce102c6b8e4\nmai_llama=$<TARGET_PROPERTY:mai_llama,INTERFACE_LINK_LIBRARIES>\nllama=$<TARGET_PROPERTY:llama,INTERFACE_LINK_LIBRARIES>\nmtmd=$<TARGET_PROPERTY:mtmd,INTERFACE_LINK_LIBRARIES>\nggml=$<TARGET_PROPERTY:ggml,INTERFACE_LINK_LIBRARIES>\n")
