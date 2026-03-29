# Find Cap'n Proto
find_package(CapnProto REQUIRED)

# Generate C++ code from Cap'n Proto schema
capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS raftpp.capnp)

# Create library with generated Cap'n Proto code
add_library(raftpp-proto ${CAPNP_SRCS} ${CAPNP_HDRS})

# Include directories for generated headers
target_include_directories(raftpp-proto PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Link Cap'n Proto libraries
target_link_libraries(raftpp-proto PUBLIC
    CapnProto::capnp
    CapnProto::capnp-rpc
    CapnProto::kj
    CapnProto::kj-async
)

# Link C++17 polyfills
target_link_libraries(raftpp-proto PUBLIC nonstd::expected-lite nonstd::span-lite)
