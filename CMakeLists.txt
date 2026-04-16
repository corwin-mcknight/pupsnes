cmake_minimum_required(VERSION 3.24)

project(pupsnes LANGUAGES CXX)

set(PUPSNES_LIB_SOURCES
  src/pupsnes/5a22/cpu.cpp
  src/pupsnes/cartridge.cpp
  src/pupsnes/scheduler.cpp
  src/pupsnes/snes.cpp
  src/pupsnes/token.cpp
  src/pupsnes/systembus.cpp
  src/pupsnes/wram.cpp
)

set(PUPSNES_CLI_SOURCES
  src/cli/main.cpp
)

set(PUPSNES_TEST_SOURCES
  src/tests/unit/catch2_smoke_tests.cpp
  src/tests/cpu_tests.cpp
  src/tests/rom_integration_tests.cpp
  src/tests/scheduler_tests.cpp
  src/tests/test_rom_builder_tests.cpp
  src/tests/token_tests.cpp
  src/tests/mem_tests.cpp
  src/tests/systembus_tests.cpp
)

set(PUPSNES_PROFILE "PRODUCTION" CACHE STRING "Build profile: DEBUG, CI, or PRODUCTION")
set_property(CACHE PUPSNES_PROFILE PROPERTY STRINGS DEBUG CI PRODUCTION)

add_library(pupsnes STATIC ${PUPSNES_LIB_SOURCES})
target_compile_features(pupsnes PUBLIC cxx_std_20)
target_include_directories(pupsnes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/includes)
target_compile_definitions(pupsnes PUBLIC "PUPSNES_PROFILE_${PUPSNES_PROFILE}")

if(PUPSNES_CLI_SOURCES)
  add_executable(pupsnes_cli ${PUPSNES_CLI_SOURCES})
  target_compile_features(pupsnes_cli PRIVATE cxx_std_20)
  target_include_directories(pupsnes_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/includes)
  target_link_libraries(pupsnes_cli PRIVATE pupsnes)
  set_target_properties(pupsnes_cli PROPERTIES OUTPUT_NAME pupsnes)
endif()

# Treat third-party (Conan) include dirs as SYSTEM so clang-tidy / warnings
# don't report issues inside vendor headers or macro expansions from them.
function(pupsnes_mark_system target)
  if(TARGET ${target})
    get_target_property(_inc ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
      set_target_properties(${target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
    endif()
  endif()
endfunction()

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
    -Wconversion
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
if(TARGET pupsnes_cli)
  pupsnes_apply_warnings(pupsnes_cli)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  pupsnes_mark_system(Catch2::Catch2)
  pupsnes_mark_system(Catch2::Catch2WithMain)
  find_program(CA65_EXECUTABLE NAMES ca65 REQUIRED)
  find_program(LD65_EXECUTABLE NAMES ld65 REQUIRED)

  set(PUPSNES_TEST_ROM_DIR ${CMAKE_CURRENT_BINARY_DIR}/test-roms)
  set(PUPSNES_TEST_ROM_CFG ${CMAKE_CURRENT_SOURCE_DIR}/testroms/lorom-32k.cfg)
  set(PUPSNES_TEST_ROM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/reset_smoke/reset_smoke.s
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/instruction_smoke/instruction_smoke.s
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/wram_signature/wram_signature.s
  )
  set(PUPSNES_TEST_ROM_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/pupsnes.inc
  )
  set(PUPSNES_TEST_ROM_METADATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/testroms)
  set(PUPSNES_TEST_ROM_OUTPUTS "")

  foreach(PUPSNES_TEST_ROM_SOURCE IN LISTS PUPSNES_TEST_ROM_SOURCES)
    get_filename_component(PUPSNES_TEST_ROM_NAME ${PUPSNES_TEST_ROM_SOURCE} NAME_WE)
    set(PUPSNES_TEST_ROM_OBJECT ${PUPSNES_TEST_ROM_DIR}/${PUPSNES_TEST_ROM_NAME}.o)
    set(PUPSNES_TEST_ROM_OUTPUT ${PUPSNES_TEST_ROM_DIR}/${PUPSNES_TEST_ROM_NAME}.sfc)
    add_custom_command(
      OUTPUT ${PUPSNES_TEST_ROM_OBJECT}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${PUPSNES_TEST_ROM_DIR}
      COMMAND ${CA65_EXECUTABLE} --cpu 65816 -I ${CMAKE_CURRENT_SOURCE_DIR}/testroms
              -o ${PUPSNES_TEST_ROM_OBJECT} ${PUPSNES_TEST_ROM_SOURCE}
      DEPENDS ${PUPSNES_TEST_ROM_SOURCE} ${PUPSNES_TEST_ROM_COMMON_INCLUDES}
      COMMENT "Assembling test ROM object ${PUPSNES_TEST_ROM_NAME}.o"
      VERBATIM
    )
    add_custom_command(
      OUTPUT ${PUPSNES_TEST_ROM_OUTPUT}
      COMMAND ${LD65_EXECUTABLE} -C ${PUPSNES_TEST_ROM_CFG} -o ${PUPSNES_TEST_ROM_OUTPUT} ${PUPSNES_TEST_ROM_OBJECT}
      DEPENDS ${PUPSNES_TEST_ROM_OBJECT} ${PUPSNES_TEST_ROM_CFG}
      COMMENT "Linking test ROM ${PUPSNES_TEST_ROM_NAME}.sfc"
      VERBATIM
    )
    list(APPEND PUPSNES_TEST_ROM_OUTPUTS ${PUPSNES_TEST_ROM_OUTPUT})
  endforeach()

  add_custom_target(pupsnes_test_roms DEPENDS ${PUPSNES_TEST_ROM_OUTPUTS})

  add_executable(pupsnes_tests ${PUPSNES_TEST_SOURCES})
  target_compile_features(pupsnes_tests PRIVATE cxx_std_20)
  target_include_directories(pupsnes_tests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/includes)
  target_link_libraries(pupsnes_tests PRIVATE pupsnes Catch2::Catch2WithMain)
  target_compile_definitions(pupsnes_tests PRIVATE
    "PUPSNES_TEST_ROM_DIR=\"${PUPSNES_TEST_ROM_DIR}\""
    "PUPSNES_TEST_ROM_METADATA_DIR=\"${PUPSNES_TEST_ROM_METADATA_DIR}\""
  )
  pupsnes_apply_warnings(pupsnes_tests)
  add_dependencies(pupsnes_tests pupsnes_test_roms)

  enable_testing()
  add_test(NAME pupsnes_tests COMMAND pupsnes_tests)
endif()

find_program(CLANG_FORMAT_EXE NAMES clang-format)
find_program(CLANG_TIDY_EXE NAMES clang-tidy)
find_program(CPPLINT_EXE NAMES cpplint)

set(PUPSNES_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/config.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/types.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/mem.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/5a22/cpu.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/cartridge.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/device.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/scheduler.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/snes.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/systembus.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/token.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/wram.h
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
    COMMAND ${CLANG_TIDY_EXE} --quiet -p=${CMAKE_BINARY_DIR} ${PUPSNES_TIDY_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running clang-tidy"
  )
else()
  add_custom_target(tidy
    COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found"
  )
endif()

if(CPPLINT_EXE)
  add_custom_target(cpplint
    COMMAND ${CPPLINT_EXE} --quiet ${PUPSNES_LINT_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running cpplint"
  )
else()
  add_custom_target(cpplint
    COMMAND ${CMAKE_COMMAND} -E echo "cpplint not found"
  )
endif()

add_custom_target(lint
  DEPENDS format-check tidy cpplint
)
