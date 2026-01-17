CPMAddPackage(
  URI "https://github.com/gabime/spdlog.git@1.16.0"
  OPTIONS
    "SPDLOG_BUILD_SHARED OFF"
    "SPDLOG_FMT_EXTERNAL OFF"
)
CPMAddPackage("https://github.com/jeremy-rifkin/libassert.git@2.2.1")
CPMAddPackage("https://github.com/libuv/libuv.git@1.51.0")
CPMAddPackage(URI "https://github.com/abseil/abseil-cpp.git" GIT_TAG "20250512.1")

if (MSVC)
  set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "Link Protobuf dynamically" FORCE)
endif ()

CPMAddPackage(
  URI "https://github.com/protocolbuffers/protobuf.git@33.4"
  OPTIONS
    "protobuf_INSTALL OFF"
    "protobuf_ABSL_PROVIDER module"
    "protobuf_BUILD_TESTS OFF"
)

if (RAFTPP_BUILD_TESTING)
  CPMAddPackage(URI "https://github.com/doctest/doctest.git@2.4.12")
endif ()

add_subdirectory(kcp)
