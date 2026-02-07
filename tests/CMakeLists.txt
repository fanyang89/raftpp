add_subdirectory(harness)
add_subdirectory(datadriven)

add_executable(
  raftpp-tests
  test_main.cc
  test_util.cc
  inflights_test.cc
  log_unstable_test.cc
  log_test.cc
  progress_test.cc
  raw_node_test.cc
  read_only_test.cc
  storage_test.cc
  raft_paper_test.cc
  raft_test.cc
  raft_snap_test.cc
  raft_flow_control_test.cc
  wal_test.cc
  rpc_test.cc
  rpc_transport_test.cc
  raftor_test.cc)

target_link_libraries(raftpp-tests PRIVATE raftpp-test-harness)
target_link_libraries(raftpp-tests PRIVATE raftor)

target_link_libraries(raftpp-tests PRIVATE doctest)
target_compile_definitions(raftpp-tests PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS=1)
target_compile_definitions(raftpp-tests PUBLIC DOCTEST_CONFIG_SUPER_FAST_ASSERTS=1)

if (RAFTPP_WITH_RDMA)
  target_sources(raftpp-tests PRIVATE rdma_transport_test.cc)
endif ()
