add_library(raftpp
  ack_indexer.cc
  conf_changer.cc
  conf_restore.cc
  error.cc
  inflights.cc
  joint_conf.cc
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
  util.cc
  # WAL implementation
  wal/crc32c.cc
  wal/record.cc
  wal/segment.cc
  wal/segment_manager.cc
  wal/wal_index.cc
  wal/metadata_store.cc
  wal/wal.cc
  wal/wal_storage.cc
)

target_include_directories(raftpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_link_libraries(raftpp PUBLIC raftpp-proto)
target_link_libraries(raftpp PUBLIC magic_enum::magic_enum)
target_link_libraries(raftpp PUBLIC spdlog)
target_link_libraries(raftpp PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(raftpp PUBLIC libassert::assert)

add_subdirectory(raftor)
add_subdirectory(rpc)
