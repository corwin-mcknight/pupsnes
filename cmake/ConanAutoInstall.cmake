# ConanAutoInstall.cmake
#
# Automatically runs `conan install` when the conan toolchain file is missing
# or when conanfile.txt has changed. Included via CMAKE_PROJECT_TOP_LEVEL_INCLUDES
# in CMakePresets.json so that `cmake --preset <name>` just works without a
# separate manual conan step.

set(_conan_toolchain "${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
set(_conanfile "${CMAKE_SOURCE_DIR}/conanfile.txt")

# Decide whether conan install needs to run
set(_need_conan FALSE)
if(NOT EXISTS "${_conan_toolchain}")
  set(_need_conan TRUE)
  message(STATUS "[ConanAutoInstall] Toolchain not found — running conan install")
elseif("${_conanfile}" IS_NEWER_THAN "${_conan_toolchain}")
  set(_need_conan TRUE)
  message(STATUS "[ConanAutoInstall] conanfile.txt changed — re-running conan install")
endif()

if(_need_conan)
  find_program(_conan_exe conan)
  if(NOT _conan_exe)
    message(FATAL_ERROR
      "[ConanAutoInstall] conan executable not found.\n"
      "Install it with: pip install conan"
    )
  endif()

  # Map CMAKE_BUILD_TYPE to the conan -s build_type setting
  if(DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "")
    set(_conan_build_type "${CMAKE_BUILD_TYPE}")
  else()
    set(_conan_build_type "Release")
  endif()

  message(STATUS "[ConanAutoInstall] conan install ${CMAKE_SOURCE_DIR} "
    "--output-folder=${CMAKE_BINARY_DIR} --build=missing "
    "-s build_type=${_conan_build_type}")

  execute_process(
    COMMAND "${_conan_exe}" install "${CMAKE_SOURCE_DIR}"
      "--output-folder=${CMAKE_BINARY_DIR}"
      "--build=missing"
      "-s" "build_type=${_conan_build_type}"
    RESULT_VARIABLE _conan_result
  )

  if(NOT _conan_result EQUAL 0)
    message(FATAL_ERROR "[ConanAutoInstall] conan install failed (exit ${_conan_result})")
  endif()

  message(STATUS "[ConanAutoInstall] conan install succeeded")

  # Conan generates CMakeUserPresets.json in the source dir with hardcoded paths
  # to build directories.  If those directories are later deleted, CMake will
  # fail at preset loading before this script can run.  Remove the file so it
  # cannot go stale.
  file(REMOVE "${CMAKE_SOURCE_DIR}/CMakeUserPresets.json")
endif()

# Include the conan-generated toolchain
if(EXISTS "${_conan_toolchain}")
  include("${_conan_toolchain}")
else()
  message(FATAL_ERROR "[ConanAutoInstall] conan_toolchain.cmake not found after install")
endif()

unset(_conan_toolchain)
unset(_conanfile)
unset(_need_conan)
unset(_conan_exe)
unset(_conan_build_type)
unset(_conan_result)
