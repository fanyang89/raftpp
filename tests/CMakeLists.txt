add_subdirectory(data)
add_subdirectory(data-driven)

target_link_libraries(raftpp-tests PRIVATE raftpp)
target_link_libraries(raftpp-tests PRIVATE raftpp-proto)

target_link_libraries(raftpp-tests PRIVATE raftpp-data-driven)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-data-conf-change)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-data-quorum)
target_link_libraries(raftpp-tests PRIVATE raftpp-test-harness)

target_link_libraries(raftpp-tests PRIVATE doctest)
target_compile_definitions(raftpp-tests PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS=1)
target_compile_definitions(raftpp-tests PUBLIC DOCTEST_CONFIG_SUPER_FAST_ASSERTS=1)
