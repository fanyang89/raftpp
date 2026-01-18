add_library(raftor-rpc
  codec.cc
  peer_manager.cc
  tcp_transport.cc
  kcp_transport.cc
  ikcp.c)

target_include_directories(raftor-rpc PUBLIC
  ${RAFTPP_INCLUDE_DIR}
  ${RAFTPP_INCLUDE_DIR}/raftpp/raftor/rpc)

target_link_libraries(raftor-rpc PUBLIC
  raftpp-proto
  uv_a
  spdlog
  libassert::assert)
