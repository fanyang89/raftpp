add_subdirectory(data)

add_executable(raftpp-tests
  test_main.cc
  data_driven_test.cc

  log_unstable_test.cc
  conf_change_data_test.cc
  progress_test.cc
  json_test.cc
)

target_link_libraries(raftpp-tests PRIVATE raftpp)
target_link_libraries(raftpp-tests PRIVATE raftpp-proto)
target_link_libraries(raftpp-tests PRIVATE gtest gmock)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-data-conf-change)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-data-quorum)
