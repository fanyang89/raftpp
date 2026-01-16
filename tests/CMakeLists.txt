add_subdirectory(harness)
add_subdirectory(datadriven)

add_executable(raftpp-tests
  test_main.cc
  test_util.cc

  inflights_test.cc
  json_test.cc
  log_unstable_test.cc
  log_test.cc
  progress_test.cc
  raw_node_test.cc
  raw_node_test2.cc
  storage_test.cc
  raft_paper_test.cc
  raft_test.cc
  raft_snap_test.cc
  raft_flow_control_test.cc
)

target_link_libraries(raftpp-tests PRIVATE raftpp)
target_link_libraries(raftpp-tests PRIVATE raftpp-proto)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-harness)

target_link_libraries(raftpp-tests PRIVATE doctest)
target_compile_definitions(raftpp-tests PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS=1)
target_compile_definitions(raftpp-tests PUBLIC DOCTEST_CONFIG_SUPER_FAST_ASSERTS=1)
