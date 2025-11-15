set(FETCHCONTENT_QUIET OFF)

CPMAddPackage(
  URI
    "https://github.com/grpc/grpc.git#v1.76.0"
  OPTIONS
    "protobuf_INSTALL OFF"
    "utf8_range_ENABLE_INSTALL OFF"
)

if (RAFTPP_BUILD_TESTING)
  CPMAddPackage("https://github.com/doctest/doctest.git#v2.4.12")
endif ()
