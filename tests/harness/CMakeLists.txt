add_library(raftpp-test-harness
  interface.cc
  network.cc
  test_util.cc
)

target_link_libraries(raftpp-test-harness PUBLIC raftpp)

target_include_directories(raftpp-test-harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(raftpp-test-harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../")
