add_library(raftor-rpc
  codec.cc
  peer_manager.cc
  capnp_transport.cc)

if (RAFTPP_WITH_RDMA)
  target_sources(raftor-rpc PRIVATE rdma_transport.cc)
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
  opentelemetry-cpp::api)
