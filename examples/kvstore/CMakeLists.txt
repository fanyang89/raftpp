CPMAddPackage("gh:yhirose/cpp-httplib#v0.14.0")
CPMAddPackage("gh:nlohmann/json@3.11.3")

add_executable(kvstore-example
  main.cc
  kv_store_state_machine.cc
  http_server.cc
)

target_link_libraries(kvstore-example PRIVATE raftor)
target_link_libraries(kvstore-example PRIVATE httplib::httplib)
target_link_libraries(kvstore-example PRIVATE nlohmann_json::nlohmann_json)

add_executable(kvstore-cli
  cli.cc
  cli_options.cc
)

target_link_libraries(kvstore-cli PRIVATE httplib::httplib)
target_link_libraries(kvstore-cli PRIVATE nlohmann_json::nlohmann_json)

if(RAFTPP_BUILD_TESTING)
  add_executable(kvstore-test test/kv_store_test.cc)
  target_link_libraries(kvstore-test PRIVATE raftor)
  target_link_libraries(kvstore-test PRIVATE nlohmann_json::nlohmann_json)
  target_link_libraries(kvstore-test PRIVATE raftpp-test-harness)
endif()
