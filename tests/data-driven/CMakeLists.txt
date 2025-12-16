add_library(raftpp-data-driven
    test_data.cc
    line_parser.cc
    test_reader.cc
    data_driven.cc
    exceptions.cc
)

target_include_directories(raftpp-data-driven PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raftpp-data-driven PRIVATE gtest)
