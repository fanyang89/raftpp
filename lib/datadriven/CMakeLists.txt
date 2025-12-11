# Data Driven Testing Framework Library
cmake_minimum_required(VERSION 3.30)

# 创建 datadriven 库
add_library(raftpp_datadriven
    test_data.cc
    line_parser.cc
    test_reader.cc
    datadriven.cc
    exceptions.cc
)

# 设置目标属性
target_include_directories(raftpp_datadriven
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# 设置 C++ 标准
target_compile_features(raftpp_datadriven PUBLIC cxx_std_23)

# 链接依赖
target_link_libraries(raftpp_datadriven
    PUBLIC
        doctest::doctest
)

# 设置编译选项
target_compile_options(raftpp_datadriven PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# 安装配置
install(TARGETS raftpp_datadriven
    EXPORT raftpp_datadriven_targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/raftpp/datadriven
    DESTINATION include/raftpp
)

# 导出目标
install(EXPORT raftpp_datadriven_targets
    FILE raftpp_datadriven_targets.cmake
    NAMESPACE raftpp::
    DESTINATION lib/cmake/raftpp
)