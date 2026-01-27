add_library(raftor-rpc
  codec.cc
  peer_manager.cc
  capnp_transport.cc)

target_include_directories(raftor-rpc PUBLIC
  ${RAFTPP_INCLUDE_DIR}
  ${RAFTPP_INCLUDE_DIR}/raftpp/raftor/rpc)

target_link_libraries(raftor-rpc PUBLIC
  raftpp)

target_link_libraries(raftor-rpc PUBLIC
  raftpp-proto
  CapnProto::capnp-rpc
  CapnProto::kj-async
  spdlog
  libassert::assert
  absl::flat_hash_map
  absl::flat_hash_set
  opentelemetry-cpp::api)
