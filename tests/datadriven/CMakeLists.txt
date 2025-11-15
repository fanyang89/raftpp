add_library(data-driven STATIC datadriven.cc)

target_link_libraries(data-driven PUBLIC doctest)
target_compile_definitions(data-driven PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS=1)
target_compile_definitions(data-driven PUBLIC DOCTEST_CONFIG_SUPER_FAST_ASSERTS=1)

add_executable(data-driven-tests
  test_main.cc
  quorum_test.cc
  confchange_test.cc)

target_link_libraries(data-driven-tests PRIVATE data-driven)
target_link_libraries(data-driven-tests PRIVATE raftor)

# doctest
target_link_libraries(data-driven-tests PRIVATE doctest)

# Test data directory
target_compile_definitions(data-driven-tests PRIVATE TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
