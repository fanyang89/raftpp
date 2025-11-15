add_executable(raftpp-tests
  tracker_test.cc
)
target_link_libraries(raftpp-tests PUBLIC doctest::doctest)
