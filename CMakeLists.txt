cmake_minimum_required(VERSION 3.24)

project(pupsnes LANGUAGES CXX)

set(PUPSNES_LIB_SOURCES
  src/pupsnes/5a22/cpu.cpp
  src/pupsnes/5a22/cpu_opcodes.cpp
  src/pupsnes/5a22/micro_op_strings.cpp
  src/pupsnes/cartridge.cpp
  src/pupsnes/scheduler.cpp
  src/pupsnes/snes.cpp
  src/pupsnes/token.cpp
  src/pupsnes/systembus.cpp
  src/pupsnes/wram.cpp
)

set(PUPSNES_DEBUGGER_CORE_SOURCES
  src/debugger/breakpoints.cpp
  src/debugger/bus_event_log.cpp
  src/debugger/disasm.cpp
  src/debugger/error_log.cpp
  src/debugger/microop_trace.cpp
  src/debugger/run_control.cpp
  src/debugger/trace.cpp
)

set(PUPSNES_DEBUGGER_SOURCES
  src/debugger/app.cpp
  src/debugger/main.cpp
  src/debugger/panels/bus_event_panel.cpp
  src/debugger/panels/controls_panel.cpp
  src/debugger/panels/disasm_panel.cpp
  src/debugger/panels/errors_panel.cpp
  src/debugger/panels/memory_panel.cpp
  src/debugger/panels/microop_trace_panel.cpp
  src/debugger/panels/ppu_panel.cpp
  src/debugger/panels/registers_panel.cpp
  src/debugger/panels/scheduler_panel.cpp
  src/debugger/panels/stack_panel.cpp
  src/debugger/panels/trace_panel.cpp
  src/debugger/third_party/imgui_glfw_backend.cpp
  src/debugger/third_party/imgui_opengl3_backend.cpp
  src/debugger/third_party/imgui_stdlib_binding.cpp
)

set(PUPSNES_TEST_SOURCES
  src/tests/unit/catch2_smoke_tests.cpp
  src/tests/cpu_tests.cpp
  src/tests/cpu_opcode_defs_tests.cpp
  src/tests/rom_integration_tests.cpp
  src/tests/scheduler_tests.cpp
  src/tests/test_rom_builder_tests.cpp
  src/tests/token_tests.cpp
  src/tests/mem_tests.cpp
  src/tests/systembus_tests.cpp
  src/tests/breakpoints_tests.cpp
  src/tests/bus_event_log_tests.cpp
  src/tests/disasm_tests.cpp
  src/tests/error_log_tests.cpp
  src/tests/run_control_tests.cpp
  src/tests/microop_trace_tests.cpp
  src/tests/opcode_alu_abs_tests.cpp
  src/tests/opcode_alu_dpx_tests.cpp
  src/tests/opcode_alu_sr_tests.cpp
  src/tests/opcode_bcd_tests.cpp
)

set(PUPSNES_PROFILE "PRODUCTION" CACHE STRING "Build profile: DEBUG, CI, or PRODUCTION")
set_property(CACHE PUPSNES_PROFILE PROPERTY STRINGS DEBUG CI PRODUCTION)

add_library(pupsnes STATIC ${PUPSNES_LIB_SOURCES})
target_compile_features(pupsnes PUBLIC cxx_std_20)
target_include_directories(pupsnes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/includes)
target_include_directories(pupsnes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(pupsnes PUBLIC "PUPSNES_PROFILE_${PUPSNES_PROFILE}")

add_library(pupsnes_debugger_core STATIC ${PUPSNES_DEBUGGER_CORE_SOURCES})
target_compile_features(pupsnes_debugger_core PUBLIC cxx_std_20)
target_include_directories(pupsnes_debugger_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/includes)
target_include_directories(pupsnes_debugger_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pupsnes_debugger_core PUBLIC pupsnes)

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

find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
pupsnes_mark_system(imgui::imgui)
pupsnes_mark_system(glfw)
get_target_property(PUPSNES_IMGUI_INCLUDE_DIR imgui::imgui INTERFACE_INCLUDE_DIRECTORIES)
list(GET PUPSNES_IMGUI_INCLUDE_DIR 0 PUPSNES_IMGUI_INCLUDE_DIR)
string(REGEX REPLACE "^\\$<\\$<CONFIG:[^>]+>:(.*)>$" "\\1" PUPSNES_IMGUI_INCLUDE_DIR "${PUPSNES_IMGUI_INCLUDE_DIR}")
get_filename_component(PUPSNES_IMGUI_ROOT "${PUPSNES_IMGUI_INCLUDE_DIR}" DIRECTORY)

add_executable(pupsnes_debugger ${PUPSNES_DEBUGGER_SOURCES})
target_compile_features(pupsnes_debugger PRIVATE cxx_std_20)
target_include_directories(pupsnes_debugger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/includes)
target_include_directories(pupsnes_debugger PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${PUPSNES_IMGUI_ROOT}/res/bindings
  ${PUPSNES_IMGUI_ROOT}/res/misc/cpp
)
target_link_libraries(pupsnes_debugger PRIVATE pupsnes_debugger_core imgui::imgui glfw OpenGL::GL)
set_source_files_properties(
  src/debugger/third_party/imgui_glfw_backend.cpp
  src/debugger/third_party/imgui_opengl3_backend.cpp
  src/debugger/third_party/imgui_stdlib_binding.cpp
  PROPERTIES COMPILE_OPTIONS "-w"
)
set_target_properties(pupsnes_debugger PROPERTIES OUTPUT_NAME pupsnes-debugger)

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
pupsnes_apply_warnings(pupsnes_debugger_core)
pupsnes_apply_warnings(pupsnes_debugger)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/stack_push/stack_push.s
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/abs_store_smoke/abs_store_smoke.s
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/inc_dec_smoke/inc_dec_smoke.s
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/flag_ops_smoke/flag_ops_smoke.s
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/transfer_smoke/transfer_smoke.s
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/control_flow_smoke/control_flow_smoke.s
    ${CMAKE_CURRENT_SOURCE_DIR}/testroms/all_instr_loop/all_instr_loop.s
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
  target_include_directories(pupsnes_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(pupsnes_tests PRIVATE pupsnes pupsnes_debugger_core Catch2::Catch2WithMain)
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
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/5a22/opcode_metadata.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/debugger/breakpoints.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/debugger/bus_event_log.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/debugger/disasm.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/debugger/error_log.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/debugger/microop_trace.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/debugger/run_control.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/debugger/trace.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/debugger/app.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/debugger/panels/panels.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/5a22/cpu.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/5a22/cpu_regs.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/5a22/micro_op_strings.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/bus_event.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/cartridge.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/debugger_contract.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/device.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/scheduler.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/snes.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/systembus.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/token.h
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/pupsnes/hw/wram.h
)

set(PUPSNES_ALL_SOURCES
  ${PUPSNES_LIB_SOURCES}
  ${PUPSNES_DEBUGGER_CORE_SOURCES}
  ${PUPSNES_DEBUGGER_SOURCES}
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
list(FILTER PUPSNES_LINT_FILES EXCLUDE REGEX ".*/src/debugger/third_party/.*")

set(PUPSNES_TIDY_FILES ${PUPSNES_ABS_SOURCES})
list(FILTER PUPSNES_TIDY_FILES INCLUDE REGEX ".*\\.(c|cc|cpp|cxx)$")
list(FILTER PUPSNES_TIDY_FILES EXCLUDE REGEX ".*/src/debugger/third_party/.*")

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
