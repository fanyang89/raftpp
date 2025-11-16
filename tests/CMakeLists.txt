add_executable(raftpp-tests
  test_main.cc
  log_unstable_test.cc
  progress_test.cc
  json_test.cc
)

target_link_libraries(raftpp-tests PRIVATE raftpp)
target_link_libraries(raftpp-tests PRIVATE raftpp-proto)
target_link_libraries(raftpp-tests PRIVATE doctest::doctest)
target_link_libraries(raftpp-tests PRIVATE nlohmann_json::nlohmann_json)
