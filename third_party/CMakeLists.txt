CPMAddPackage(
  URI "https://github.com/jeremy-rifkin/cpptrace.git"
  GIT_TAG "3db8da80111171c219ab5839905771386bee06b3")

CPMAddPackage(
  URI "https://github.com/jeremy-rifkin/libassert.git@2.2.1"
  OPTIONS "LIBASSERT_USE_EXTERNAL_CPPTRACE ON")

CPMAddPackage(
  URI "https://github.com/gabime/spdlog.git@1.16.0"
  OPTIONS
    "SPDLOG_BUILD_SHARED OFF"
    "SPDLOG_FMT_EXTERNAL OFF")
CPMAddPackage(
  URI "https://github.com/abseil/abseil-cpp.git"
  GIT_TAG "20260107.1")

if(RAFTPP_BUILD_TESTING)
  CPMAddPackage(URI "https://github.com/fanyang89/doctest.git" GIT_TAG master)
endif()

CPMAddPackage(
  URI "https://github.com/google/crc32c.git"
  GIT_TAG "1.1.2"
  OPTIONS
    "CMAKE_POLICY_VERSION_MINIMUM 3.5"
    "CRC32C_BUILD_TESTS OFF"
    "CRC32C_BUILD_BENCHMARKS OFF"
    "CRC32C_USE_GLOG OFF"
    "CRC32C_INSTALL OFF"
  PATCHES
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/crc32c-arm64-alignment.patch
)

# Cap'n Proto for serialization and RPC
CPMAddPackage(
  URI "https://github.com/capnproto/capnproto.git@1.3.0"
  OPTIONS
    "BUILD_TESTING OFF"
    "WITH_OPENSSL OFF"
    "WITH_FIBERS OFF"
  PATCHES
    ${CMAKE_CURRENT_SOURCE_DIR}/patches/capnproto-no-fibers-asan.patch
)

# OpenTelemetry API (header-only, no-op by default)
CPMAddPackage(
  URI "https://github.com/fanyang89/opentelemetry-cpp-api.git"
  GIT_TAG "main"
  OPTIONS
    "OPENTELEMETRY_INSTALL OFF"
    "WITH_STL CXX17"
    "WITH_NO_DEPRECATED_CODE ON"
)

# C++17 polyfills
CPMAddPackage(
  URI "https://github.com/nonstd-lite/expected-lite.git"
  GIT_TAG "v0.10.0"
)
CPMAddPackage(
  URI "https://github.com/nonstd-lite/span-lite.git"
  GIT_TAG "v0.11.0"
)
