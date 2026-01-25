cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(raftpp
  VERSION 0.1.0
  DESCRIPTION "C++ RAFT"
  LANGUAGES C CXX)

option(RAFTPP_BUILD_TESTING "Build unit tests" ON)
option(RAFTPP_BUILD_EXAMPLES "Build examples" ON)
option(RAFTPP_WITH_CCACHE "Build with ccache" ON)
option(RAFTPP_WITH_LLD "Build with lld" OFF)
option(RAFTPP_SANITIZE "Build with address sanitizer" OFF)
option(RAFTPP_ENABLE_UBSAN "Build with undefined sanitizer when address sanitizer is enabled" ON)
option(RAFTPP_COVERAGE "Build with code coverage instrumentation" OFF)

# cpm.cmake cache directory
# https://github.com/cpm-cmake/CPM.cmake/issues/563
set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cache/cpm" CACHE STRING "CPM cache path")
include(cmake/CPM.cmake)

if (RAFTPP_WITH_LLD)
  find_program(LLD_FOUND lld)
  if (LLD_FOUND)
    add_link_options(-fuse-ld=lld)
  else ()
    message(WARNING "lld not found but RAFTPP_WITH_LLD is enabled")
  endif ()
endif ()

if (RAFTPP_SANITIZE)
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
add_subdirectory(lib)
add_subdirectory(proto)
if (RAFTPP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif ()

if (RAFTPP_BUILD_TESTING)
  add_subdirectory(tests)
endif ()

if (RAFTPP_COVERAGE)
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(raftpp PUBLIC
      -fprofile-instr-generate
      -fcoverage-mapping)
    target_link_options(raftpp PUBLIC
      -fprofile-instr-generate
      -fcoverage-mapping)
  else ()
    target_compile_options(raftpp PUBLIC --coverage)
    target_link_options(raftpp PUBLIC --coverage)
  endif ()
endif ()
