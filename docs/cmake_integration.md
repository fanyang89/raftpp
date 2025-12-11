# CMake 集成计划

## 概述

本文档描述了如何将 data driven 测试框架集成到现有的 raftpp 项目中，包括目录结构、构建配置和依赖管理。

## 目录结构

### 新增目录结构

```
raftpp/
├── include/raftpp/datadriven/          # 新增：头文件目录
│   ├── test_data.h                     # 核心数据结构
│   ├── line_parser.h                   # 行解析器
│   ├── test_reader.h                   # 测试数据读取器
│   ├── datadriven.h                    # 主测试运行器
│   └── exceptions.h                    # 异常定义
├── lib/datadriven/                     # 新增：源文件目录
│   ├── CMakeLists.txt                 # datadriven 库构建配置
│   ├── test_data.cc                   # 核心数据结构实现
│   ├── line_parser.cc                 # 行解析器实现
│   ├── test_reader.cc                 # 测试数据读取器实现
│   ├── datadriven.cc                 # 主测试运行器实现
│   └── exceptions.cc                  # 异常实现
├── tests/
│   ├── CMakeLists.txt                 # 更新：添加 datadriven 测试
│   ├── datadriven_test.cc             # 新增：data driven 测试示例
│   └── testdata/                     # 新增：测试数据目录
│       ├── math_operations/
│       │   ├── fibonacci.txt
│       │   └── basic_arithmetic.txt
│       ├── string_operations/
│       │   ├── concat.txt
│       │   └── split.txt
│       └── raft_algorithm/
│           ├── leader_election.txt
│           └── log_replication.txt
└── docs/                            # 已有：文档目录
    ├── datadriven_design.md
    ├── implementation_plan.md
    ├── test_data_examples.md
    └── cmake_integration.md
```

## CMake 配置文件

### 1. 主 CMakeLists.txt 更新

在根目录的 `CMakeLists.txt` 中添加：

```cmake
# 在现有 add_subdirectory 调用后添加
if (RAFTPP_BUILD_TESTING)
    add_subdirectory(lib/datadriven)  # 新增
    add_subdirectory(tests)
endif()
```

### 2. lib/datadriven/CMakeLists.txt

```cmake
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
        nlohmann_json::nlohmann_json
    PRIVATE
        # 可能需要的其他依赖
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
```

### 3. tests/CMakeLists.txt 更新

```cmake
# 更新现有的测试配置
add_executable(raftpp-tests
    test_main.cc
    log_unstable_test.cc
    progress_test.cc
    json_test.cc
    datadriven_test.cc  # 新增
)

# 更新链接库
target_link_libraries(raftpp-tests PRIVATE 
    raftpp
    raftpp-proto
    raftpp_datadriven  # 新增
    doctest::doctest
    nlohmann_json::nlohmann_json
)

# 添加测试数据目录
add_test(NAME raftpp-unit-tests COMMAND raftpp-tests)

# 为 data driven 测试添加单独的测试目标
add_test(NAME raftpp-datadriven-tests 
         COMMAND raftpp-tests 
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
```

## 依赖管理

### 现有依赖复用

项目已经包含以下依赖，data driven 框架将复用：

1. **doctest** - 测试框架
2. **nlohmann_json** - JSON 处理（如果需要）
3. **spdlog** - 日志记录（如果需要）

### 新增依赖

如果需要额外的依赖，将通过 CPM.cmake 添加：

```cmake
# 在 lib/datadriven/CMakeLists.txt 中
CPMAddPackage(
    NAME regex
    VERSION 2.0.0
    GITHUB_REPOSITORY "llvm/llvm-project"
    GIT_TAG "llvmorg-15.0.0"
)
```

## 构建配置

### 编译选项

```cmake
# 在 lib/datadriven/CMakeLists.txt 中
option(RAFTPP_DATADRIVEN_BUILD_EXAMPLES "Build data driven test examples" ON)
option(RAFTPP_DATADRIVEN_ENABLE_BENCHMARKS "Enable performance benchmarks" OFF)
option(RAFTPP_DATADRIVEN_VERBOSE_LOGGING "Enable verbose logging for debugging" OFF)

# 条件编译
if (RAFTPP_DATADRIVEN_VERBOSE_LOGGING)
    target_compile_definitions(raftpp_datadriven PRIVATE DATADRIVEN_VERBOSE_LOGGING)
endif()
```

### 测试配置

```cmake
# 在 tests/CMakeLists.txt 中
if (RAFTPP_DATADRIVEN_BUILD_EXAMPLES)
    # 添加示例测试可执行文件
    add_executable(datadriven_examples
        examples/math_operations.cc
        examples/string_operations.cc
        examples/raft_algorithm.cc
    )
    
    target_link_libraries(datadriven_examples
        PRIVATE
            raftpp_datadriven
            raftpp
    )
    
    add_test(NAME datadriven-examples COMMAND datadriven_examples)
endif()
```

## 安装和打包

### 安装配置

```cmake
# 在根目录的 CMakeLists.txt 中
include(GNUInstallDirs)

# 安装 datadriven 组件
if (RAFTPP_BUILD_TESTING)
    install(DIRECTORY tests/testdata/
            DESTINATION share/raftpp/testdata
            FILES_MATCHING PATTERN "*.txt")
endif()
```

### 打包配置

```cmake
# CPack 配置
set(CPACK_COMPONENT_DATADRIVEN_DISPLAY_NAME "Data Driven Testing Framework")
set(CPACK_COMPONENT_DATADRIVEN_DESCRIPTION "Framework for data driven unit testing")
set(CPACK_COMPONENT_DATADRIVEN_REQUIRED True)
```

## 开发工作流

### 1. 开发环境设置

```bash
# 克隆项目
git clone <repository_url>
cd raftpp

# 创建构建目录
mkdir build && cd build

# 配置 CMake
cmake .. -DRAFTPP_BUILD_TESTING=ON

# 构建
make -j$(nproc)

# 运行测试
ctest --output-on-failure
```

### 2. 添加新测试

1. 在 `tests/testdata/` 中创建测试数据文件
2. 在 `tests/datadriven_test.cc` 中添加测试函数
3. 更新 CMakeLists.txt（如果需要）
4. 运行测试验证

### 3. 调试和开发

```bash
# 启用详细日志
cmake .. -DRAFTPP_BUILD_TESTING=ON -DRAFTPP_DATADRIVEN_VERBOSE_LOGGING=ON

# 启用基准测试
cmake .. -DRAFTPP_DATADRIVEN_ENABLE_BENCHMARKS=ON

# 构建调试版本
cmake .. -DCMAKE_BUILD_TYPE=Debug -DRAFTPP_BUILD_TESTING=ON
```

## 持续集成

### GitHub Actions 配置

```yaml
# .github/workflows/test.yml
name: Test

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v3
    
    - name: Configure CMake
      run: cmake -B build -DRAFTPP_BUILD_TESTING=ON
    
    - name: Build
      run: cmake --build build --parallel
    
    - name: Test
      run: |
        cd build
        ctest --output-on-failure --verbose
```

## 性能优化

### 编译时优化

```cmake
# 优化编译选项
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(raftpp_datadriven PRIVATE
        -O3
        -DNDEBUG
        -march=native
    )
endif()
```

### 链接时优化

```cmake
# 启用 LTO（链接时优化）
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if (ipo_supported)
        set_property(TARGET raftpp_datadriven PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()
```

## 文档生成

### Doxygen 配置

```cmake
# 在 lib/datadriven/CMakeLists.txt 中
find_package(Doxygen QUIET)
if (DOXYGEN_FOUND)
    set(DOXYGEN_INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    set(DOXYGEN_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/docs)
    
    doxygen_add_docs(datadriven_docs
        ${DOXYGEN_INPUT_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generate documentation for datadriven framework"
    )
endif()
```

这个 CMake 集成计划确保了 data driven 测试框架能够无缝集成到现有的 raftpp 项目中，同时保持代码的模块化和可维护性。