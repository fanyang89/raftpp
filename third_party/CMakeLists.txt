CPMAddPackage("https://github.com/Neargye/magic_enum.git@0.9.7")
CPMAddPackage(
  URI "https://github.com/gabime/spdlog.git@1.16.0"
  OPTIONS
    "SPDLOG_BUILD_SHARED OFF"
    "SPDLOG_FMT_EXTERNAL OFF"
)
CPMAddPackage("https://github.com/jeremy-rifkin/libassert.git@2.2.1")
CPMAddPackage(URI "https://github.com/nlohmann/json.git@3.12.0" OPTIONS "JSON_Install off")
CPMAddPackage("https://github.com/libuv/libuv.git@1.51.0")

# Ensure spdlog uses the same C++ standard as the parent project
# if(TARGET spdlog)
#   set_target_properties(spdlog PROPERTIES
#     CXX_STANDARD 23
#     CXX_STANDARD_REQUIRED ON
#   )
#   # Force rebuild with correct settings
#   target_compile_options(spdlog PRIVATE -std=c++23)
# endif()

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

# KCP library (local)
add_subdirectory(kcp)
