add_library(raftor-wal
  crc32c.cc
  metadata_store.cc
  record.cc
  segment.cc
  segment_manager.cc
  wal.cc
  wal_index.cc
  wal_storage.cc)

target_include_directories(raftor-wal PUBLIC ${RAFTPP_INCLUDE_DIR})

target_link_libraries(raftor-wal PUBLIC
  raftpp)

target_link_libraries(raftor-wal PRIVATE
  raftpp-proto
  crc32c
  opentelemetry-cpp::api)

if(RAFTPP_WITH_LIBURING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET uring::uring)
  target_link_libraries(raftor-wal PRIVATE uring::uring)
  target_compile_definitions(raftor-wal PRIVATE RAFTPP_HAS_LIBURING=1)
else()
  target_compile_definitions(raftor-wal PRIVATE RAFTPP_HAS_LIBURING=0)
endif()
