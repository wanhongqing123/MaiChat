cmake_minimum_required(VERSION 3.16)

# 宿主先提供同一份 Core 与 SQLite；不另找系统数据库或复制媒体/模型依赖。
if(NOT TARGET maiagent OR NOT TARGET mai_sqlite)
    message(FATAL_ERROR "MaiChat History requires existing maiagent and mai_sqlite targets")
endif()

add_library(mai_im_history STATIC MaiIMHistorySnapshot.cpp MaiIMHistoryFormat.cpp)
add_library(MaiChat::IMHistory ALIAS mai_im_history)
target_compile_features(mai_im_history PUBLIC cxx_std_17)
target_include_directories(mai_im_history PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/nlohmann
            ${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/sqlite)
target_link_libraries(mai_im_history PUBLIC maiagent PRIVATE mai_sqlite)
set_target_properties(mai_im_history PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(mai_im_history PRIVATE /utf-8)
endif()

option(MAI_IM_HISTORY_BUILD_TESTS "Build owned local SQLite history reader tests" OFF)
if(MAI_IM_HISTORY_BUILD_TESTS)
    add_executable(mai_im_history_snapshot_test tests/MaiIMHistorySnapshotTest.cpp)
    target_compile_features(mai_im_history_snapshot_test PRIVATE cxx_std_17)
    target_include_directories(mai_im_history_snapshot_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../MaiAgent/third_party/sqlite)
    target_link_libraries(mai_im_history_snapshot_test PRIVATE mai_im_history mai_sqlite)
    add_test(NAME mai_im_history_snapshot_test COMMAND mai_im_history_snapshot_test)
endif()
