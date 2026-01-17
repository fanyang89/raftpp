add_library(raftpp-wal
  crc32c.cc
  record.cc
  segment.cc
  segment_manager.cc
  wal_index.cc
  metadata_store.cc
  wal.cc
  wal_storage.cc
)

target_include_directories(raftpp-wal PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
)

target_link_libraries(raftpp-wal PRIVATE
  raftpp-proto
  crc32c
  libassert::assert
  spdlog::spdlog
)
