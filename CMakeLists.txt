cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(raftpp VERSION 0.1.0 DESCRIPTION "C++ RAFT" LANGUAGES C CXX)

option(RAFTPP_BUILD_TESTING "Build unit tests" ON)
option(RAFTPP_WITH_CCACHE "Build with ccache" ON)
option(RAFTPP_WITH_LLD "Build with lld" OFF)
option(RAFTPP_SANITIZE "Build with address sanitizer" OFF)
option(RAFTPP_ENABLE_UBSAN "Build with undefined sanitizer when address sanitizer is enabled" ON)

# cpm.cmake cache directory
# https://github.com/cpm-cmake/CPM.cmake/issues/563
set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cache/cpm" CACHE STRING "CPM cache path")

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

# On macOS with Homebrew LLVM, ensure we link against Homebrew's libc++
if (APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  get_filename_component(LLVM_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(LLVM_DIR "${LLVM_BIN_DIR}" DIRECTORY)
  if (EXISTS "${LLVM_DIR}/lib/c++/libc++.dylib")
    add_link_options("-L${LLVM_DIR}/lib/c++" "-Wl,-rpath,${LLVM_DIR}/lib/c++")
  endif ()
endif ()

include(cmake/CPM.cmake)
add_subdirectory(third_party)
add_subdirectory(lib)
add_subdirectory(proto)

if (RAFTPP_BUILD_TESTING)
  add_subdirectory(tests)
endif ()
