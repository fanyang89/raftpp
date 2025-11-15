add_library(raftpp
  lib.cc
)

target_include_directories(raftpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
