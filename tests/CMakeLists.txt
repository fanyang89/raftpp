add_subdirectory(data)
add_subdirectory(data-driven)
add_subdirectory(harness)

add_executable(raftpp-tests
  test_main.cc
  test_util.cc

  data_driven_tests.cc
  inflights_test.cc
  json_test.cc
  log_unstable_test.cc
  log_test.cc
  progress_test.cc
  raw_node_test.cc
  storage_test.cc
  network_test.cc
 )

target_link_libraries(raftpp-tests PRIVATE raftpp)
target_link_libraries(raftpp-tests PRIVATE raftpp-proto)

target_link_libraries(raftpp-tests PRIVATE raftpp-data-driven)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-data-conf-change)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-data-quorum)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-harness)

target_link_libraries(raftpp-tests PRIVATE doctest)
target_compile_definitions(raftpp-tests PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS=1)
target_compile_definitions(raftpp-tests PUBLIC DOCTEST_CONFIG_SUPER_FAST_ASSERTS=1)
