add_library(raftpp
  config.cc
  error.cc
  joint.cc
  log_unstable.cc
  logging.cc
  majority.cc
  progress.cc
  quorum.cc
  raft.cc
  raft_core.cc
  raft_log.cc
  raw_node.cc
  readonly.cc
  storage.cc
  tracker.cc
  util.cc
  conf_restore.cc
  conf_changer.cc
  tracker_conf.cc
)

target_include_directories(raftpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(raftpp PUBLIC raftpp-proto)
target_link_libraries(raftpp PUBLIC magic_enum::magic_enum)
target_link_libraries(raftpp PUBLIC Boost::circular_buffer)
target_link_libraries(raftpp PUBLIC spdlog)
target_link_libraries(raftpp PUBLIC nlohmann_json::nlohmann_json)

target_link_libraries(raftpp PRIVATE sqlite3)
target_link_libraries(raftpp PRIVATE libassert::assert)
