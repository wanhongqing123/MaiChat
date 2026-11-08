# Build one pinned source SDK, then import its public targets. Qt's own CMake
# directory settings stay outside the MaiChat/MaiAgent target hierarchy.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(MAICHAT_QT_BUILD_JOBS 4 CACHE STRING "Parallel jobs for the bundled Qt SDK")
set(_maichat_qt_builder "${CMAKE_CURRENT_LIST_DIR}/../scripts/build-qt.py")
set(_maichat_qt_args --cmake "${CMAKE_COMMAND}" --jobs "${MAICHAT_QT_BUILD_JOBS}"
  --arch "${CMAKE_SYSTEM_PROCESSOR}"
  --c-compiler "${CMAKE_C_COMPILER}" --cxx-compiler "${CMAKE_CXX_COMPILER}")
if(APPLE)
  if(CMAKE_OSX_ARCHITECTURES)
    list(APPEND _maichat_qt_args --arch "${CMAKE_OSX_ARCHITECTURES}")
  endif()
  if(CMAKE_OSX_SYSROOT)
    list(APPEND _maichat_qt_args --sysroot "${CMAKE_OSX_SYSROOT}")
  endif()
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND _maichat_qt_args --deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_maichat_qt_builder}"
  ${_maichat_qt_args} --print-prefix
  OUTPUT_VARIABLE MAICHAT_QT_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_maichat_qt_builder}"
  ${_maichat_qt_args} COMMAND_ERROR_IS_FATAL ANY)
# Do not silently pick up a system Qt 5/6 SDK when the source bootstrap is missing.
set(Qt6_DIR "${MAICHAT_QT_PREFIX}/lib/cmake/Qt6" CACHE PATH "Bundled Qt source SDK" FORCE)
find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS
  Core Widgets Svg Concurrent Test
  PATHS "${Qt6_DIR}" NO_DEFAULT_PATH)
