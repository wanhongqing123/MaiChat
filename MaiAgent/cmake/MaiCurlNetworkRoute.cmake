# 模型和普通网络库链接同一个租约状态，避免静态库各自维护一份网络代次。
if(NOT TARGET mai_curl_network_route)
  add_library(mai_curl_network_route STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../Agent/MaiCurlNetworkRoute.cpp")
  target_compile_features(mai_curl_network_route PUBLIC cxx_std_17)
  target_include_directories(mai_curl_network_route PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../include")
  target_link_libraries(mai_curl_network_route PUBLIC CURL::libcurl)
  if(WIN32)
    target_link_libraries(mai_curl_network_route PRIVATE ws2_32)
  endif()
  set_target_properties(mai_curl_network_route PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
