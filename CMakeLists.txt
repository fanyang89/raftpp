cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(raftpp
  VERSION 0.1.0
  DESCRIPTION "C++ RAFT"
  LANGUAGES C CXX)

function(raftpp_enforce_cxx17 target_name)
  if (TARGET ${target_name})
    set_target_properties(${target_name} PROPERTIES
      CXX_STANDARD 17
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
  endif ()
endfunction()

function(raftpp_enable_project_warnings target_name)
  if (TARGET ${target_name} AND RAFTPP_ENABLE_WARNINGS)
    target_compile_options(${target_name} PRIVATE
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wall>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wextra>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wunused>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wunused-variable>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wunused-parameter>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wunused-function>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wunused-value>
      $<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wunreachable-code>)
  endif ()
endfunction()

function(raftpp_enable_iwyu target_name)
  if (TARGET ${target_name} AND RAFTPP_ENABLE_IWYU)
    set_property(TARGET ${target_name} PROPERTY
      CXX_INCLUDE_WHAT_YOU_USE "${RAFTPP_IWYU_BIN}")
  endif ()
endfunction()

function(raftpp_apply_to_targets function_name)
  foreach(target_name IN ITEMS ${ARGN})
    cmake_language(CALL ${function_name} ${target_name})
  endforeach()
endfunction()

function(raftpp_resolve_iwyu)
  if (NOT RAFTPP_ENABLE_IWYU)
    return()
  endif ()

  if (RAFTPP_IWYU_EXECUTABLE AND IS_ABSOLUTE "${RAFTPP_IWYU_EXECUTABLE}")
    if (NOT EXISTS "${RAFTPP_IWYU_EXECUTABLE}")
      message(FATAL_ERROR "RAFTPP_IWYU_EXECUTABLE does not exist: ${RAFTPP_IWYU_EXECUTABLE}")
    endif ()
    set(raftpp_iwyu_bin "${RAFTPP_IWYU_EXECUTABLE}")
  elseif (RAFTPP_IWYU_EXECUTABLE)
    find_program(raftpp_iwyu_bin NAMES "${RAFTPP_IWYU_EXECUTABLE}" REQUIRED)
  else ()
    find_program(raftpp_iwyu_bin NAMES include-what-you-use iwyu REQUIRED)
  endif ()

  set(RAFTPP_IWYU_BIN "${raftpp_iwyu_bin}" PARENT_SCOPE)
endfunction()

function(raftpp_detect_liburing)
  set(raftpp_liburing_deps_available OFF)

  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
      pkg_check_modules(LIBURING QUIET IMPORTED_TARGET liburing)
      if (TARGET PkgConfig::LIBURING)
        set(raftpp_liburing_deps_available ON)
        if (NOT TARGET uring::uring)
          add_library(uring::uring INTERFACE IMPORTED GLOBAL)
          target_link_libraries(uring::uring INTERFACE PkgConfig::LIBURING)
        endif ()
      endif ()
    endif ()
  endif ()

  set(RAFTPP_LIBURING_DEPS_AVAILABLE ${raftpp_liburing_deps_available} PARENT_SCOPE)
endfunction()

function(raftpp_validate_liburing)
  if (NOT RAFTPP_WITH_LIBURING)
    return()
  endif ()

  if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "RAFTPP_WITH_LIBURING is ON but liburing is Linux-only")
  endif ()

  if (NOT RAFTPP_LIBURING_DEPS_AVAILABLE)
    message(FATAL_ERROR "RAFTPP_WITH_LIBURING is ON but system liburing was not found via pkg-config")
  endif ()
endfunction()

option(RAFTPP_BUILD_TESTING "Build unit tests" ON)
option(RAFTPP_BUILD_EXAMPLES "Build examples" ON)
option(RAFTPP_WITH_CCACHE "Build with ccache" ON)
option(RAFTPP_WITH_LLD "Build with lld" OFF)
option(RAFTPP_ENABLE_WARNINGS "Enable project compiler warnings" OFF)
option(RAFTPP_ENABLE_IWYU "Run include-what-you-use during C++ builds" OFF)
option(RAFTPP_USE_EXTERNAL_FMT "Use external/system fmt instead of vendored fmt" OFF)
set(RAFTPP_IWYU_EXECUTABLE "" CACHE FILEPATH "Path to the include-what-you-use executable")
raftpp_detect_liburing()
option(RAFTPP_WITH_LIBURING "Build with system liburing discovered via pkg-config" ${RAFTPP_LIBURING_DEPS_AVAILABLE})
set(RAFTPP_SANITIZE "" CACHE STRING "Sanitizer to enable (address or thread)")
set_property(CACHE RAFTPP_SANITIZE PROPERTY STRINGS "" "address" "thread")
option(RAFTPP_ENABLE_UBSAN "Build with undefined sanitizer when address sanitizer is enabled" ON)
option(RAFTPP_COVERAGE "Build with code coverage instrumentation" OFF)

# cpm.cmake cache directory
# https://github.com/cpm-cmake/CPM.cmake/issues/563
set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cache/cpm" CACHE STRING "CPM cache path")
include(cmake/CPM.cmake)

raftpp_resolve_iwyu()

if (RAFTPP_WITH_LLD)
  find_program(LLD_FOUND lld)
  if (LLD_FOUND)
    add_link_options(-fuse-ld=lld)
  else ()
    message(WARNING "lld not found but RAFTPP_WITH_LLD is enabled")
  endif ()
endif ()

raftpp_validate_liburing()

set(RAFTPP_SUPPORTED_SANITIZERS address thread)
if (RAFTPP_SANITIZE)
  if (NOT RAFTPP_SANITIZE IN_LIST RAFTPP_SUPPORTED_SANITIZERS)
    message(FATAL_ERROR "RAFTPP_SANITIZE must be one of: address, thread")
  endif ()

  add_compile_options(-fsanitize=${RAFTPP_SANITIZE})
  add_link_options(-fsanitize=${RAFTPP_SANITIZE})
  if (RAFTPP_SANITIZE STREQUAL "address" AND RAFTPP_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
  endif ()
endif ()

if (MSVC)
  add_compile_options(/Zc:preprocessor)
  add_compile_options(/utf-8)
  add_compile_options(/MP)
endif ()

set(RAFTPP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(third_party)

if (RAFTPP_COVERAGE)
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
  else ()
    message(WARNING "RAFTPP_COVERAGE with GCC: use gcov/lcov to generate reports (task coverage requires Clang)")
    add_compile_options(--coverage)
    add_link_options(--coverage)
  endif ()
endif ()

add_subdirectory(lib)
add_subdirectory(proto)
if (RAFTPP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif ()

if (RAFTPP_BUILD_TESTING)
  add_subdirectory(tests)
endif ()

set(RAFTPP_CONFIGURED_TARGETS
  raftpp-proto
  raftpp
  raftor-rpc
  raftor-wal
  raftor
  raftpp-test-harness
  data-driven
  data-driven-tests
  raftpp-tests
  kvstore-example
  kvstore-cli
  kvstore-test
  minimal-node-example)

set(RAFTPP_IWYU_TARGETS ${RAFTPP_CONFIGURED_TARGETS})
list(REMOVE_ITEM RAFTPP_IWYU_TARGETS raftpp-proto)

raftpp_apply_to_targets(raftpp_enforce_cxx17 ${RAFTPP_CONFIGURED_TARGETS})
raftpp_apply_to_targets(raftpp_enable_project_warnings ${RAFTPP_CONFIGURED_TARGETS})
raftpp_apply_to_targets(raftpp_enable_iwyu ${RAFTPP_IWYU_TARGETS})
