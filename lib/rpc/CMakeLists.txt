add_library(raftpp-rpc
    codec.cc
    peer_manager.cc
    tcp_transport.cc
    kcp_transport.cc
)

target_include_directories(raftpp-rpc PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/kcp
)

target_link_libraries(raftpp-rpc
    PUBLIC
        raftpp-proto
        uv_a
        spdlog
        kcp
        libassert::assert
)
