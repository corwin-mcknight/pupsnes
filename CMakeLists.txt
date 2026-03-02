cmake_minimum_required(VERSION 3.23)

project(pupsnes LANGUAGES CXX)

file(GLOB_RECURSE PUPSNES_LIB_SOURCES CONFIGURE_DEPENDS
  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
  "src/pupsnes/*.c"
  "src/pupsnes/*.cc"
  "src/pupsnes/*.cpp"
  "src/pupsnes/*.cxx"
)

file(GLOB_RECURSE PUPSNES_CLI_SOURCES CONFIGURE_DEPENDS
  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
  "src/cli/*.c"
  "src/cli/*.cc"
  "src/cli/*.cpp"
  "src/cli/*.cxx"
)

file(GLOB_RECURSE PUPSNES_TEST_SOURCES CONFIGURE_DEPENDS
  RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
  "src/tests/*.c"
  "src/tests/*.cc"
  "src/tests/*.cpp"
  "src/tests/*.cxx"
)

add_library(pupsnes STATIC ${PUPSNES_LIB_SOURCES})
target_compile_features(pupsnes PUBLIC cxx_std_20)
target_include_directories(pupsnes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/includes)

add_executable(pupsnes_cli ${PUPSNES_CLI_SOURCES})
target_compile_features(pupsnes_cli PUBLIC cxx_std_20)
target_include_directories(pupsnes_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/includes)
target_link_libraries(pupsnes_cli PRIVATE pupsnes)
set_target_properties(pupsnes_cli PROPERTIES OUTPUT_NAME pupsnes)

find_package(zlib CONFIG REQUIRED)
target_link_libraries(pupsnes PUBLIC ZLIB::ZLIB)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PUPSNES_WERROR "Treat warnings as errors" OFF)

include(CheckCXXCompilerFlag)
set(CMAKE_REQUIRED_QUIET TRUE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(CMAKE_REQUIRED_FLAGS "-Werror")
endif()

function(pupsnes_add_flag target flag)
  string(REPLACE "-" "_" _flag_id "${flag}")
  string(REPLACE "=" "_" _flag_id "${_flag_id}")
  string(REPLACE "+" "_" _flag_id "${_flag_id}")
  string(REPLACE "," "_" _flag_id "${_flag_id}")
  string(REPLACE ":" "_" _flag_id "${_flag_id}")
  string(REPLACE "/" "_" _flag_id "${_flag_id}")
  string(REPLACE " " "_" _flag_id "${_flag_id}")
  string(REPLACE "(" "_" _flag_id "${_flag_id}")
  string(REPLACE ")" "_" _flag_id "${_flag_id}")
  string(REPLACE "." "_" _flag_id "${_flag_id}")
  string(REPLACE "*" "_" _flag_id "${_flag_id}")
  string(REPLACE ";" "_" _flag_id "${_flag_id}")
  string(REPLACE "\\" "_" _flag_id "${_flag_id}")
  string(TOUPPER "${_flag_id}" _flag_id)
  set(_flag_var "PUPSNES_HAS_${_flag_id}")
  check_cxx_compiler_flag("${flag}" "${_flag_var}")
  if(${_flag_var})
    target_compile_options(${target} PRIVATE "${flag}")
  endif()
endfunction()

function(pupsnes_apply_warnings target)
  foreach(PUPSNES_WARN_FLAG IN ITEMS
    -Wall
    -Wextra
    -Wpedantic
    -Wconversation
    -Wshadow
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
  )
    pupsnes_add_flag(${target} "${PUPSNES_WARN_FLAG}")
  endforeach()

  if(PUPSNES_WERROR)
    pupsnes_add_flag(${target} -Werror)
  endif()
endfunction()

pupsnes_apply_warnings(pupsnes)
pupsnes_apply_warnings(pupsnes_cli)

include(CTest)
if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)

  add_executable(pupsnes_tests ${PUPSNES_TEST_SOURCES})
  target_compile_features(pupsnes_tests PUBLIC cxx_std_20)
  target_include_directories(pupsnes_tests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/includes)
  target_compile_definitions(pupsnes_tests PRIVATE PUPSNES_TESTING=1)
  target_link_libraries(pupsnes_tests PRIVATE pupsnes Catch2::Catch2WithMain)
  pupsnes_apply_warnings(pupsnes_tests)

  enable_testing()
  add_test(NAME pupsnes_tests COMMAND pupsnes_tests)
endif()

find_program(CLANG_FORMAT_EXE NAMES clang-format)
find_program(CLANG_TIDY_EXE NAMES clang-tidy)

file(GLOB_RECURSE PUPSNES_HEADERS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/includes/cli/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/includes/cli/*.hpp"
)

set(PUPSNES_ALL_SOURCES
  ${PUPSNES_LIB_SOURCES}
  ${PUPSNES_CLI_SOURCES}
  ${PUPSNES_TEST_SOURCES}
)

set(PUPSNES_ABS_SOURCES "")
foreach(PUPSNES_SOURCE IN LISTS PUPSNES_ALL_SOURCES)
  if(IS_ABSOLUTE "${PUPSNES_SOURCE}")
    list(APPEND PUPSNES_ABS_SOURCES "${PUPSNES_SOURCE}")
  else()
    list(APPEND PUPSNES_ABS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${PUPSNES_SOURCE}")
  endif()
endforeach()

set(PUPSNES_LINT_FILES ${PUPSNES_ABS_SOURCES} ${PUPSNES_HEADERS})

set(PUPSNES_TIDY_FILES ${PUPSNES_ABS_SOURCES})
list(FILTER PUPSNES_TIDY_FILES INCLUDE REGEX ".*\\.(c|cc|cpp|cxx)$")

if(CLANG_FORMAT_EXE)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXE} -i ${PUPSNES_LINT_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running clang-format"
  )

  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${PUPSNES_LINT_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking clang-format"
  )
else()
  add_custom_target(format
    COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
  )
  add_custom_target(format-check
    COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
  )
endif()

if(CLANG_TIDY_EXE)
  add_custom_target(tidy
    COMMAND ${CLANG_TIDY_EXE} -p=${CMAKE_BINARY_DIR} ${PUPSNES_TIDY_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running clang-tidy"
  )
else()
  add_custom_target(tidy
    COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found"
  )
endif()

add_custom_target(lint
  DEPENDS format-check tidy
)
