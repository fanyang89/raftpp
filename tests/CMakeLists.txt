add_subdirectory(data)
add_subdirectory(data-driven)

add_executable(raftpp-tests
  test_main.cc

  data_driven_tests.cc
  inflights_test.cc
  json_test.cc
  log_unstable_test.cc
  progress_test.cc
  raw_node_test.cc
  storage_test.cc
)

target_link_libraries(raftpp-tests PRIVATE raftpp)
target_link_libraries(raftpp-tests PRIVATE raftpp-proto)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-data-conf-change)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-data-quorum)
target_link_libraries(raftpp-tests PRIVATE raftpp-data-driven)

target_link_libraries(raftpp-tests PRIVATE doctest)
