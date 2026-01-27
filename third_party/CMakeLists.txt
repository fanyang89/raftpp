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
  GIT_TAG "20250512.1")

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
)

if(RAFTPP_WITH_LIBURING)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(ExternalProject)
    set(LIBURING_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/liburing-install")
    ExternalProject_Add(
      liburing_ep
      GIT_REPOSITORY "https://github.com/axboe/liburing.git"
      GIT_TAG "liburing-2.12"
      CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env CC=${CMAKE_C_COMPILER} ./configure
        --prefix=${LIBURING_INSTALL_DIR} --disable-shared --enable-static
      BUILD_COMMAND make
      INSTALL_COMMAND make install
      BUILD_IN_SOURCE 1
    )
    add_library(uring::uring STATIC IMPORTED)
    set_target_properties(
      uring::uring
      PROPERTIES
        IMPORTED_LOCATION "${LIBURING_INSTALL_DIR}/lib/liburing.a"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBURING_INSTALL_DIR}/include"
    )
    add_dependencies(uring::uring liburing_ep)
  else()
    message(
      WARNING
        "RAFTPP_WITH_LIBURING is ON but liburing is Linux-only; disabling."
    )
    set(RAFTPP_WITH_LIBURING OFF CACHE BOOL "Enable liburing" FORCE)
  endif()
endif()

# OpenTelemetry API (header-only, no-op by default)
CPMAddPackage(
  URI "https://github.com/fanyang89/opentelemetry-cpp-api.git"
  GIT_TAG "main"
  OPTIONS
    "OPENTELEMETRY_INSTALL OFF"
    "WITH_STL CXX23"
    "WITH_NO_DEPRECATED_CODE ON"
)
