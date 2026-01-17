add_library(raftor-rpc
  codec.cc
  peer_manager.cc
  tcp_transport.cc
  kcp_transport.cc
)

target_include_directories(raftor-rpc PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/third_party/kcp
)

target_link_libraries(raftor-rpc PUBLIC
  raftpp-proto
  uv_a
  spdlog
  kcp
  libassert::assert
)
