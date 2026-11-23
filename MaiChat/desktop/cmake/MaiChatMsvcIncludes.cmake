# A localized /showIncludes marker can have different encodings in cl's output
# and CMake's generated Ninja rules. That silently records zero dependencies and
# mixes old object layouts with new constructors after public headers change.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  set(_maichat_include_launcher "${CMAKE_CURRENT_LIST_DIR}/../scripts/msvc-includes.py")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_maichat_include_launcher}"
      --probe "${CMAKE_CXX_COMPILER}"
    OUTPUT_VARIABLE _maichat_include_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  foreach(_maichat_language C CXX)
    set(CMAKE_${_maichat_language}_CL_SHOWINCLUDES_PREFIX "Note: including file:")
    # Retain an explicitly configured launcher (for example a compiler cache).
    set(CMAKE_${_maichat_language}_COMPILER_LAUNCHER
      "${Python3_EXECUTABLE}" "${_maichat_include_launcher}"
      --include-prefix-hex "${_maichat_include_prefix}" --
      ${CMAKE_${_maichat_language}_COMPILER_LAUNCHER})
  endforeach()
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file:")
  # Qt/third-party project and language setup can reload cached compiler metadata
  # later in this directory. Restore the marker after that setup, before Ninja
  # generation; the compiler launcher is inherited by all ordinary consumers.
  cmake_language(DEFER CALL set CMAKE_C_CL_SHOWINCLUDES_PREFIX "Note: including file:")
  cmake_language(DEFER CALL set CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "Note: including file:")
  cmake_language(DEFER CALL set CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file:")
endif()
