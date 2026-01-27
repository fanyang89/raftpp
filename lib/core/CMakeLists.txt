add_library(raftpp
  ack_indexer.cc
  conf_changer.cc
  conf_restore.cc
  error.cc
  inflights.cc
  joint_conf.cc
  logging.cc
  majority_conf.cc
  memory_storage.cc
  progress_tracker.cc
  progress.cc
  raft_config.cc
  raft_core.cc
  raft_log.cc
  raft.cc
  raw_node.cc
  read_only.cc
  storage.cc
  tracker_conf.cc
  unstable_log.cc
  util.cc)

target_include_directories(raftpp PUBLIC ${RAFTPP_INCLUDE_DIR})

target_link_libraries(raftpp PUBLIC raftpp-proto)
target_link_libraries(raftpp PUBLIC spdlog)
target_link_libraries(raftpp PUBLIC opentelemetry-cpp::api)
target_link_libraries(raftpp PUBLIC libassert::assert)
target_link_libraries(raftpp PUBLIC absl::flat_hash_map absl::flat_hash_set)
