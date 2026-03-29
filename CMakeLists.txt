cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 17)
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
set(RAFTPP_LIBURING_DEPS_AVAILABLE OFF)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules(LIBURING QUIET IMPORTED_TARGET liburing)
    if (TARGET PkgConfig::LIBURING)
      set(RAFTPP_LIBURING_DEPS_AVAILABLE ON)
      add_library(uring::uring INTERFACE IMPORTED GLOBAL)
      target_link_libraries(uring::uring INTERFACE PkgConfig::LIBURING)
    endif ()
  endif ()
endif ()
option(RAFTPP_WITH_LIBURING "Build with system liburing discovered via pkg-config" ${RAFTPP_LIBURING_DEPS_AVAILABLE})
set(RAFTPP_RDMA_DEPS_AVAILABLE OFF)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(CheckIncludeFile)
  check_include_file("rdma/rdma_cma.h" RAFTPP_HAVE_RDMACM_HEADER)
  check_include_file("infiniband/verbs.h" RAFTPP_HAVE_IBVERBS_HEADER)
  find_library(RDMACM_LIB rdmacm)
  find_library(IBVERBS_LIB ibverbs)
  if (RAFTPP_HAVE_RDMACM_HEADER AND RAFTPP_HAVE_IBVERBS_HEADER AND RDMACM_LIB AND IBVERBS_LIB)
    set(RAFTPP_RDMA_DEPS_AVAILABLE ON)
  endif ()
endif ()
option(RAFTPP_WITH_RDMA "Build RDMA transport (rdma-core)" ${RAFTPP_RDMA_DEPS_AVAILABLE})
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

if (RAFTPP_WITH_RDMA AND NOT RAFTPP_RDMA_DEPS_AVAILABLE)
  message(FATAL_ERROR "RAFTPP_WITH_RDMA is ON but rdma-core headers/libraries not found")
endif ()

if (RAFTPP_WITH_LIBURING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT RAFTPP_LIBURING_DEPS_AVAILABLE)
  message(FATAL_ERROR "RAFTPP_WITH_LIBURING is ON but system liburing was not found via pkg-config")
endif ()

if (RAFTPP_WITH_LIBURING AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "RAFTPP_WITH_LIBURING is ON but liburing is Linux-only")
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
