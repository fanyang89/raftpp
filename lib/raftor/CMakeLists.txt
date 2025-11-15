add_subdirectory(wal)
add_subdirectory(rpc)

add_library(raftor raftor.cc proposal_tracker.cc ready_processor.cc)

target_link_libraries(raftor PUBLIC raftpp raftor-rpc raftor-wal)
