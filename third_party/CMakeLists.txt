set(FETCHCONTENT_QUIET OFF)

CPMAddPackage(
  URI
    "https://github.com/grpc/grpc.git#v1.76.0"
  OPTIONS
    "protobuf_INSTALL OFF"
    "utf8_range_ENABLE_INSTALL OFF"
)

add_library(sqlite3 sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/sqlite3/include")

if (RAFTPP_BUILD_TESTING)
  CPMAddPackage("https://github.com/doctest/doctest.git#v2.4.12")
endif ()
