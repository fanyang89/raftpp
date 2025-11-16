add_library(raftpp
  error.cc
  joint.cc
  log_unstable.cc
  logging.cc
  majority.cc
  quorum.cc
  raft.cc
  raft_log.cc
  storage.cc
  util.cc
  tracker.cc
  progress.cc
  readonly.cc
  config.cc
  raw_node.cc
)

target_include_directories(raftpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(raftpp PUBLIC raftpp-proto)
target_link_libraries(raftpp PUBLIC magic_enum::magic_enum)
target_link_libraries(raftpp PUBLIC Boost::circular_buffer)
target_link_libraries(raftpp PUBLIC spdlog)

target_link_libraries(raftpp PRIVATE sqlite3)
target_link_libraries(raftpp PRIVATE libassert::assert)
target_link_libraries(raftpp PRIVATE nlohmann_json::nlohmann_json)
