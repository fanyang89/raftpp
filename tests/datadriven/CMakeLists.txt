add_library(raftpp-datadriven STATIC
    datadriven.cc
)

target_include_directories(raftpp-datadriven PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(raftpp-datadriven PUBLIC cxx_std_20)

add_executable(raftpp-datadriven-tests
    test_main.cc
    quorum_test.cc
    confchange_test.cc
)

target_link_libraries(raftpp-datadriven-tests PRIVATE raftpp-datadriven)
target_link_libraries(raftpp-datadriven-tests PRIVATE raftpp)
target_link_libraries(raftpp-datadriven-tests PRIVATE raftpp-proto)
target_link_libraries(raftpp-datadriven-tests PRIVATE doctest)

target_compile_definitions(raftpp-datadriven-tests PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS=1)
target_compile_definitions(raftpp-datadriven-tests PUBLIC DOCTEST_CONFIG_SUPER_FAST_ASSERTS=1)
