set(FETCHCONTENT_QUIET OFF)

CPMAddPackage("https://github.com/Neargye/magic_enum.git@0.9.7")
CPMAddPackage("https://github.com/bombela/backward-cpp.git@1.6")
CPMAddPackage("https://github.com/gabime/spdlog.git@1.16.0")
CPMAddPackage("https://github.com/jeremy-rifkin/libassert.git@2.2.1")
CPMAddPackage(URI "https://github.com/nlohmann/json.git@3.12.0" OPTIONS "JSON_Install off")

CPMAddPackage(URI "https://github.com/boostorg/circular_buffer.git" GIT_TAG "boost-1.89.0")
CPMAddPackage(URI "https://github.com/boostorg/assert.git" GIT_TAG "boost-1.89.0")
CPMAddPackage(URI "https://github.com/boostorg/concept_check.git" GIT_TAG "boost-1.89.0")
CPMAddPackage(URI "https://github.com/boostorg/config.git" GIT_TAG "boost-1.89.0")
CPMAddPackage(URI "https://github.com/boostorg/core.git" GIT_TAG "boost-1.89.0")
CPMAddPackage(URI "https://github.com/boostorg/move.git" GIT_TAG "boost-1.89.0")
CPMAddPackage(URI "https://github.com/boostorg/static_assert.git" GIT_TAG "boost-1.89.0")
CPMAddPackage(URI "https://github.com/boostorg/throw_exception.git" GIT_TAG "boost-1.89.0")
CPMAddPackage(URI "https://github.com/boostorg/type_traits.git" GIT_TAG "boost-1.89.0")
CPMAddPackage(URI "https://github.com/boostorg/preprocessor.git" GIT_TAG "boost-1.89.0")

CPMAddPackage(
  URI "https://github.com/grpc/grpc.git@1.76.0"
  OPTIONS
  "protobuf_INSTALL OFF"
  "utf8_range_ENABLE_INSTALL OFF"
)

if (RAFTPP_BUILD_TESTING)
  CPMAddPackage("https://github.com/doctest/doctest.git@2.4.12")
endif ()

add_library(sqlite3 sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/sqlite3/include")
