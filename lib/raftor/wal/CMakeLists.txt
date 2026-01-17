add_library(raftor-wal
  crc32c.cc
  metadata_store.cc
  record.cc
  segment.cc
  segment_manager.cc
  wal.cc
  wal_index.cc
  wal_storage.cc
)

target_include_directories(raftor-wal PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
)

target_link_libraries(raftor-wal PRIVATE
  raftpp-proto
  crc32c
  libassert::assert
  spdlog::spdlog
)
