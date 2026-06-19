add_subdirectory(wal)
add_subdirectory(rpc)

if (RAFTPP_WITH_SEASTAR)
  add_library(raftor-seastar-support INTERFACE)
  target_link_libraries(raftor-seastar-support INTERFACE Seastar::seastar)
endif ()

add_library(raftor raftor.cc mock_state_machine.cc proposal_tracker.cc ready_processor.cc std_runtime.cc)

target_link_libraries(raftor PUBLIC raftpp raftor-rpc raftor-wal opentelemetry-cpp::api)
