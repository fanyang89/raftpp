CPMAddPackage(
  URI "https://github.com/jeremy-rifkin/cpptrace.git"
  GIT_TAG "3db8da80111171c219ab5839905771386bee06b3")

CPMAddPackage(
  URI "https://github.com/jeremy-rifkin/libassert.git@2.2.1"
  OPTIONS "LIBASSERT_USE_EXTERNAL_CPPTRACE ON")

CPMAddPackage("https://github.com/libuv/libuv.git@1.51.0")

CPMAddPackage(
  URI "https://github.com/gabime/spdlog.git@1.16.0"
  OPTIONS
    "SPDLOG_BUILD_SHARED OFF"
    "SPDLOG_FMT_EXTERNAL OFF")
CPMAddPackage(
  URI "https://github.com/abseil/abseil-cpp.git"
  GIT_TAG "20250512.1")

if(MSVC)
  set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "Link Protobuf dynamically" FORCE)
endif()

CPMAddPackage(
  URI "https://github.com/protocolbuffers/protobuf.git@32.1"
  OPTIONS
    "protobuf_INSTALL OFF"
    "protobuf_ABSL_PROVIDER module"
    "protobuf_BUILD_TESTS OFF")

if(RAFTPP_BUILD_TESTING)
  CPMAddPackage(URI "https://github.com/fanyang89/doctest.git" GIT_TAG master)
endif()

# Disable UBSan for KCP library as it has false positives from its queue macros
# kcp/ikcp.c:317:10: runtime error: member access within null pointer of type 'IKCPSEG' (aka 'struct IKCPSEG')
# SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior kcp/ikcp.c:317:10
#if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
#  target_compile_options(kcp PRIVATE -fno-sanitize=undefined)
#endif()

CPMAddPackage(
  URI "https://github.com/google/crc32c.git"
  GIT_TAG "1.1.2"
  OPTIONS
    "CMAKE_POLICY_VERSION_MINIMUM 3.5"
    "CRC32C_BUILD_TESTS OFF"
    "CRC32C_BUILD_BENCHMARKS OFF"
    "CRC32C_USE_GLOG OFF"
    "CRC32C_INSTALL OFF"
)

CPMAddPackage(
  URI "https://github.com/fanyang89/rpclib.git"
  GIT_TAG "master"
)
