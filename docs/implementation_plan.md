# Data Driven 测试框架实现计划

## 实现概述

基于设计文档，我们将实现一个完整的 C++ 版本的 data driven 测试框架。该框架将完全兼容 raft-rs 的测试数据格式，并提供类型安全的 C++ 接口。

## 详细实现规范

### 1. 核心数据结构实现

#### 1.1 CmdArg 结构 (`include/raftpp/datadriven/test_data.h`)

```cpp
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <ostream>

namespace raftpp {
namespace datadriven {

struct CmdArg {
    std::string key;
    std::vector<std::string> vals;
    
    // 构造函数
    CmdArg() = default;
    CmdArg(const std::string& k, const std::vector<std::string>& v);
    
    // 便利方法
    bool HasValue() const;
    std::string GetValue() const;
    const std::vector<std::string>& GetValues() const;
    std::string ToString() const;
    
    // 比较操作符
    bool operator==(const CmdArg& other) const;
    bool operator!=(const CmdArg& other) const;
};

// 流输出操作符
std::ostream& operator<<(std::ostream& os, const CmdArg& arg);

} // namespace datadriven
} // namespace raftpp
```

#### 1.2 TestData 结构 (`include/raftpp/datadriven/test_data.h`)

```cpp
struct TestData {
    std::string pos;                    // 文件位置信息
    std::string cmd;                    // 命令名
    std::vector<CmdArg> cmd_args;       // 命令参数
    std::string input;                   // 输入数据
    std::string expected;                // 期望输出
    
    // 构造函数
    TestData() = default;
    
    // 查询方法
    bool ContainsKey(const std::string& key) const;
    std::optional<CmdArg> GetArg(const std::string& key) const;
    std::optional<std::string> GetValue(const std::string& key) const;
    std::optional<std::vector<std::string>> GetValues(const std::string& key) const;
    
    // 调试方法
    std::string DebugString() const;
};
```

### 2. 参数解析器实现

#### 2.1 LineParser 类 (`include/raftpp/datadriven/line_parser.h`)

```cpp
#pragma once

#include "test_data.h"
#include <string>
#include <vector>
#include <utility>

namespace raftpp {
namespace datadriven {

class LineParser {
public:
    // 解析单行命令
    static std::pair<std::string, std::vector<CmdArg>> ParseLine(const std::string& line);
    
private:
    // 分割指令
    static std::vector<std::string> SplitDirectives(const std::string& line);
    
    // 解析单个参数
    static CmdArg ParseArgument(const std::string& arg);
    
    // 正则表达式匹配
    static bool IsValidArgument(const std::string& arg);
};

} // namespace datadriven
} // namespace raftpp
```

#### 2.2 LineParser 实现 (`lib/datadriven/line_parser.cc`)

- 使用正则表达式解析参数格式
- 支持续行符 `\` 处理
- 错误处理和位置信息记录

### 3. 测试数据读取器实现

#### 3.1 TestDataReader 类 (`include/raftpp/datadriven/test_reader.h`)

```cpp
#pragma once

#include "test_data.h"
#include <string>
#include <optional>
#include <sstream>

namespace raftpp {
namespace datadriven {

class TestDataReader {
public:
    TestDataReader(const std::string& filename, const std::string& content, bool rewrite = false);
    
    // 读取下一个测试用例
    bool NextTest();
    
    // 获取当前测试用例
    const TestData& GetCurrentTest() const;
    
    // 获取重写缓冲区内容（如果启用重写模式）
    std::optional<std::string> GetRewriteBuffer() const;
    
private:
    void ReadExpected();
    void EmitLine(const std::string& line);
    std::string Trim(const std::string& str);
    
    std::string filename_;
    std::istringstream content_;
    TestData current_test_;
    bool rewrite_mode_;
    std::optional<std::string> rewrite_buffer_;
    int line_number_;
};

} // namespace datadriven
} // namespace raftpp
```

### 4. 主测试运行器实现

#### 4.1 DataDrivenTest 类 (`include/raftpp/datadriven/datadriven.h`)

```cpp
#pragma once

#include "test_data.h"
#include <string>
#include <functional>
#include <vector>
#include <filesystem>

namespace raftpp {
namespace datadriven {

class DataDrivenTest {
public:
    using TestFunction = std::function<std::string(const TestData&)>;
    using WalkFunction = std::function<void(const std::filesystem::path&)>;
    
    // 运行单个文件或目录中的所有测试
    static void RunTest(const std::string& path, TestFunction func, bool rewrite = false);
    
    // 遍历目录中的所有文件
    static void WalkTests(const std::string& path, WalkFunction func);
    
private:
    static void RunSingleTest(const std::string& filename, const std::string& content, 
                           TestFunction func, bool rewrite);
    static void RunDirective(TestDataReader& reader, TestFunction func);
    static bool HasBlankLine(const std::string& str);
    static std::vector<std::string> GetTestFiles(const std::string& path);
};

} // namespace datadriven
} // namespace raftpp
```

### 5. 异常处理

#### 5.1 异常类定义 (`include/raftpp/datadriven/exceptions.h`)

```cpp
#pragma once

#include <stdexcept>
#include <string>

namespace raftpp {
namespace datadriven {

class ParseException : public std::runtime_error {
public:
    ParseException(const std::string& message, const std::string& file, int line);
    
    const std::string& GetFile() const { return file_; }
    int GetLine() const { return line_; }
    
private:
    std::string file_;
    int line_;
};

} // namespace datadriven
} // namespace raftpp
```

## 实现步骤

### 第一阶段：核心数据结构
1. 实现 `test_data.h` 和 `test_data.cc`
2. 实现 `exceptions.h` 和 `exceptions.cc`
3. 编写单元测试验证数据结构

### 第二阶段：解析器
1. 实现 `line_parser.h` 和 `line_parser.cc`
2. 实现 `test_reader.h` 和 `test_reader.cc`
3. 编写解析器测试

### 第三阶段：测试运行器
1. 实现 `datadriven.h` 和 `datadriven.cc`
2. 集成 doctest 框架
3. 编写集成测试

### 第四阶段：示例和文档
1. 创建测试数据示例文件
2. 编写使用示例
3. 完善文档

## 测试策略

### 单元测试
- 每个组件的独立测试
- 边界条件和错误情况测试
- 性能基准测试

### 集成测试
- 完整的测试流程验证
- 与现有 doctest 框架的集成测试
- 实际项目中的使用案例测试

### 兼容性测试
- 与 raft-rs 测试数据格式的兼容性
- 不同平台下的兼容性测试

## 性能考虑

1. **内存管理**：使用 RAII 和智能指针
2. **字符串处理**：避免不必要的拷贝
3. **解析缓存**：可选的解析结果缓存
4. **延迟加载**：按需解析测试数据

## 扩展性设计

1. **插件架构**：支持自定义解析器
2. **钩子机制**：测试前后的自定义操作
3. **格式扩展**：支持多种测试数据格式
4. **输出格式**：支持多种输出格式

## 集成计划

### CMake 集成
- 添加新的子目录 `lib/datadriven`
- 更新主 `CMakeLists.txt`
- 添加测试目标

### doctest 集成
- 提供 doctest 友好的宏定义
- 自动注册测试用例
- 错误报告集成

### 项目集成
- 更新现有测试使用新框架
- 提供迁移指南
- 示例和最佳实践