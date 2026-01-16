add_library(raftpp-rpc
    codec.cc
    peer_manager.cc
    tcp_transport.cc
)

target_include_directories(raftpp-rpc PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(raftpp-rpc
    PUBLIC
        raftpp-proto
        uv_a
        spdlog
        absl::flat_hash_map
        absl::flat_hash_set
        nlohmann_json::nlohmann_json
)
