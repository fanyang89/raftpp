CPMAddPackage("https://github.com/Neargye/magic_enum.git@0.9.7")
CPMAddPackage("https://github.com/gabime/spdlog.git@1.16.0")
CPMAddPackage("https://github.com/jeremy-rifkin/libassert.git@2.2.1")
CPMAddPackage(URI "https://github.com/nlohmann/json.git@3.12.0" OPTIONS "JSON_Install off")
CPMAddPackage("https://github.com/google/leveldb.git@1.20")
CPMAddPackage("https://github.com/libuv/libuv.git@1.51.0")
CPMAddPackage(URI "https://github.com/abseil/abseil-cpp.git" GIT_TAG "20250814.1")

if (MSVC)
  set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "Link Protobuf dynamically" FORCE)
endif ()

CPMAddPackage(
  URI "https://github.com/protocolbuffers/protobuf.git@33.2"
  OPTIONS "protobuf_INSTALL OFF" "ABSL_ENABLE_INSTALL OFF" "protobuf_BUILD_TESTS OFF")

if (RAFTPP_BUILD_TESTING)
  CPMAddPackage(URI "https://github.com/doctest/doctest.git@2.4.12")
endif ()

add_library(sqlite3 sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/sqlite3/include")
