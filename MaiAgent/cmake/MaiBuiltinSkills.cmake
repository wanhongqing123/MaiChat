# One source of bundled instructions is compiled into the shared core on every
# platform. No per-platform copies or writes to the user's skill directories.
file(GLOB mai_builtin_skill_files CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/*/SKILL.md")
list(SORT mai_builtin_skill_files)
set(mai_builtin_skill_code "// Generated from the exact bundled SKILL.md bytes.\nnamespace {\n")
set(mai_builtin_skill_records "")
set(mai_builtin_skill_names "")
set(mai_builtin_skill_index 0)
foreach(mai_builtin_skill_file IN LISTS mai_builtin_skill_files)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${mai_builtin_skill_file}")
  file(READ "${mai_builtin_skill_file}" mai_builtin_skill_text)
  file(READ "${mai_builtin_skill_file}" mai_builtin_skill_hex HEX)
  # Normalize metadata parsing only; the embedded body retains original bytes.
  string(REPLACE "\r\n" "\n" mai_builtin_skill_text "${mai_builtin_skill_text}")
  string(LENGTH "${mai_builtin_skill_hex}" mai_builtin_skill_hex_size)
  if(mai_builtin_skill_hex_size GREATER 32768)
    message(FATAL_ERROR "Bundled Skill exceeds the 16KiB source budget: ${mai_builtin_skill_file}")
  endif()
  # Built-in metadata deliberately uses plain single-line ASCII scalars. Full
  # body/YAML validation still goes through the normal Skill manager in tests.
  string(REGEX MATCH "\nname: ([a-z0-9-]+)\n" mai_builtin_match "${mai_builtin_skill_text}")
  set(mai_builtin_skill_name "${CMAKE_MATCH_1}")
  string(REGEX MATCH "\ndescription: ([^\r\n]+)\r?\n" mai_builtin_match "${mai_builtin_skill_text}")
  set(mai_builtin_skill_description "${CMAKE_MATCH_1}")
  if(mai_builtin_skill_name STREQUAL "" OR mai_builtin_skill_description STREQUAL "")
    message(FATAL_ERROR "Missing bundled Skill metadata: ${mai_builtin_skill_file}")
  endif()
  get_filename_component(mai_builtin_skill_directory "${mai_builtin_skill_file}" DIRECTORY)
  get_filename_component(mai_builtin_skill_folder "${mai_builtin_skill_directory}" NAME)
  if(NOT mai_builtin_skill_folder STREQUAL mai_builtin_skill_name OR
     mai_builtin_skill_name IN_LIST mai_builtin_skill_names)
    message(FATAL_ERROR "Bundled Skill names must be unique and match their folder: ${mai_builtin_skill_file}")
  endif()
  list(APPEND mai_builtin_skill_names "${mai_builtin_skill_name}")
  string(REPLACE "\\" "\\\\" mai_builtin_skill_description "${mai_builtin_skill_description}")
  string(REPLACE "\"" "\\\"" mai_builtin_skill_description "${mai_builtin_skill_description}")
  string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1,\n" mai_builtin_skill_bytes "${mai_builtin_skill_hex}")
  string(APPEND mai_builtin_skill_code "const unsigned char kMaiSkill${mai_builtin_skill_index}[] = {${mai_builtin_skill_bytes}};\n")
  string(APPEND mai_builtin_skill_records
    "{\"${mai_builtin_skill_name}\", \"${mai_builtin_skill_description}\", std::string_view(reinterpret_cast<const char*>(kMaiSkill${mai_builtin_skill_index}), sizeof(kMaiSkill${mai_builtin_skill_index}))},\n")
  math(EXPR mai_builtin_skill_index "${mai_builtin_skill_index} + 1")
endforeach()
string(APPEND mai_builtin_skill_code
  "const std::vector<MaiBuiltinSkillResource> kMaiBuiltinSkillResources = {\n${mai_builtin_skill_records}};\n}\n")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated-skills")
set(mai_builtin_skill_output "${CMAKE_CURRENT_BINARY_DIR}/generated-skills/MaiBuiltinSkillResources.inc")
# configure_file avoids updating the include timestamp when inputs are unchanged.
file(WRITE "${mai_builtin_skill_output}.in" "${mai_builtin_skill_code}")
configure_file("${mai_builtin_skill_output}.in" "${mai_builtin_skill_output}" COPYONLY)
