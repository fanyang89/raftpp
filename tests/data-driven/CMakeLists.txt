add_library(raftpp-data-driven
  data_driven.cc
  data_driven_test.cc
  exceptions.cc
  line_parser.cc
  test_data.cc
  test_reader.cc
)

target_include_directories(raftpp-data-driven PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(raftpp-data-driven PRIVATE raftpp-test-data-conf-change)
target_link_libraries(raftpp-data-driven PRIVATE doctest)
target_link_libraries(raftpp-data-driven PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(raftpp-data-driven PRIVATE spdlog)
target_link_libraries(raftpp-data-driven PRIVATE absl::strings)
