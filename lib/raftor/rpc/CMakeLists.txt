add_library(raftor-rpc
  codec.cc
  peer_manager.cc
  capnp_transport.cc)

if (RAFTPP_WITH_RDMA)
  target_sources(raftor-rpc PRIVATE rdma_transport.cc)
  find_library(RDMACM_LIB rdmacm)
  find_library(IBVERBS_LIB ibverbs)
  if (NOT RDMACM_LIB OR NOT IBVERBS_LIB)
    message(FATAL_ERROR "RAFTPP_WITH_RDMA is ON but rdma-core libraries not found")
  endif()
  target_link_libraries(raftor-rpc PUBLIC ${RDMACM_LIB} ${IBVERBS_LIB})
  target_compile_definitions(raftor-rpc PUBLIC RAFTPP_WITH_RDMA=1)
else()
  target_compile_definitions(raftor-rpc PUBLIC RAFTPP_WITH_RDMA=0)
endif()

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
