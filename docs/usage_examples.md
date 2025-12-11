# Data Driven 测试框架使用示例

## 快速开始

### 1. 基本使用

```cpp
#include "raftpp/datadriven/datadriven.h"
#include <sstream>

// 定义测试函数
std::string MyTestFunction(const raftpp::datadriven::TestData& data) {
    std::string result;
    
    if (data.cmd == "echo") {
        // 简单的回显测试
        for (const auto& arg : data.cmd_args) {
            if (arg.HasValue()) {
                result += arg.key + "=" + arg.GetValue() + "\n";
            }
        }
    }
    
    return result;
}

// 在 doctest 中使用
TEST_CASE("My data driven test") {
    raftpp::datadriven::DataDrivenTest::RunTest(
        "tests/testdata/my_tests", 
        MyTestFunction
    );
}
```

### 2. 测试数据文件示例

创建 `tests/testdata/my_tests/echo.txt`：

```
# 基本回显测试
echo message=hello world
----
message=hello world

# 多参数测试
echo name=alice age=25 city=beijing
----
name=alice
age=25
city=beijing

# 空值测试
echo empty= key_only
----
empty=
key_only=
```

## 高级用法

### 1. 复杂数据处理

```cpp
std::string ProcessComplexData(const raftpp::datadriven::TestData& data) {
    std::string result;
    
    if (data.cmd == "calculate") {
        // 处理数值计算
        auto operation = data.GetValue("op");
        auto values = data.GetValues("numbers");
        
        if (operation && values) {
            if (*operation == "sum") {
                int sum = 0;
                for (const auto& val : *values) {
                    sum += std::stoi(val);
                }
                result = "result=" + std::to_string(sum);
            } else if (*operation == "product") {
                int product = 1;
                for (const auto& val : *values) {
                    product *= std::stoi(val);
                }
                result = "result=" + std::to_string(product);
            }
        }
    }
    
    return result;
}
```

### 2. 多行输出支持

```cpp
std::string GenerateMultiLineOutput(const raftpp::datadriven::TestData& data) {
    std::string result;
    
    if (data.cmd == "format") {
        auto items = data.GetValues("items");
        if (items) {
            result += "Formatted List:\n";
            result += "---------------\n";
            for (size_t i = 0; i < items->size(); ++i) {
                result += std::to_string(i + 1) + ". " + (*items)[i] + "\n";
            }
            result += "---------------\n";
            result += "Total: " + std::to_string(items->size()) + " items\n";
        }
    }
    
    return result;
}
```

### 3. 错误处理

```cpp
std::string RobustTestFunction(const raftpp::datadriven::TestData& data) {
    try {
        if (data.cmd == "divide") {
            auto numerator = data.GetValue("numerator");
            auto denominator = data.GetValue("denominator");
            
            if (!numerator || !denominator) {
                throw raftpp::datadriven::TestException(
                    "Missing required parameters", 
                    data.pos
                );
            }
            
            int num = std::stoi(*numerator);
            int den = std::stoi(*denominator);
            
            if (den == 0) {
                throw raftpp::datadriven::TestException(
                    "Division by zero", 
                    data.pos
                );
            }
            
            return "result=" + std::to_string(num / den);
        }
    } catch (const std::exception& e) {
        throw raftpp::datadriven::TestException(
            std::string("Calculation error: ") + e.what(), 
            data.pos
        );
    }
}
```

## 测试数据组织

### 1. 目录结构

```
tests/testdata/
├── basic_tests/
│   ├── echo.txt
│   ├── calculate.txt
│   └── format.txt
├── edge_cases/
│   ├── empty_input.txt
│   ├── invalid_params.txt
│   └── error_cases.txt
└── integration/
    ├── complex_workflow.txt
    └── multi_step.txt
```

### 2. 测试文件命名约定

- 使用描述性名称：`feature_name.txt`
- 避免特殊字符和空格
- 使用下划线分隔单词
- 保持名称简洁但有意义

### 3. 测试用例组织

```
# 功能测试
feature_name param1=value1 param2=value2
----
expected_output

# 边界条件测试
feature_name param=boundary_value
----
expected_boundary_output

# 错误处理测试
feature_name invalid_param=invalid_value
----
expected_error_message
```

## 重写模式使用

当需要更新测试数据时，可以使用重写模式：

```cpp
// 启用重写模式（通常用于开发阶段）
TEST_CASE("Update test data") {
    raftpp::datadriven::DataDrivenTest::RunTest(
        "tests/testdata/feature_to_update", 
        MyTestFunction,
        true  // 启用重写模式
    );
}
```

重写模式会：
1. 执行测试函数
2. 将实际输出写入测试文件
3. 替换原有的期望输出

## 最佳实践

### 1. 测试函数设计

```cpp
// ✅ 好的做法：清晰的命令处理
std::string WellStructuredTest(const raftpp::datadriven::TestData& data) {
    if (data.cmd == "command1") {
        return HandleCommand1(data);
    } else if (data.cmd == "command2") {
        return HandleCommand2(data);
    }
    
    // 提供默认错误信息
    return "error: unknown command '" + data.cmd + "'";
}

// ❌ 避免：复杂的嵌套逻辑
std::string PoorlyStructuredTest(const raftpp::datadriven::TestData& data) {
    std::string result;
    for (const auto& arg : data.cmd_args) {
        if (arg.key == "type1") {
            if (arg.GetValue() == "subtype1") {
                // 深度嵌套逻辑...
            }
        }
        // 更多嵌套...
    }
    return result;
}
```

### 2. 参数验证

```cpp
std::string ValidatedTest(const raftpp::datadriven::TestData& data) {
    // 验证必需参数
    if (!data.ContainsKey("required_param")) {
        throw raftpp::datadriven::TestException(
            "Missing required parameter: required_param",
            data.pos
        );
    }
    
    // 验证参数类型
    auto num_param = data.GetValue("number");
    if (num_param) {
        try {
            std::stoi(*num_param);
        } catch (const std::exception&) {
            throw raftpp::datadriven::TestException(
                "Invalid number format: " + *num_param,
                data.pos
            );
        }
    }
    
    // 执行实际测试逻辑
    return ProcessValidatedData(data);
}
```

### 3. 测试数据设计

```
# ✅ 好的做法：清晰的测试描述
# 测试基本加法功能
add a=5 b=3
----
8

# 测试边界条件：零值
add a=0 b=5
----
5

# 测试负数
add a=-3 b=7
----
4

# ❌ 避免：模糊的测试用例
test something something
----
some result
```

## 与现有测试集成

### 1. 迁移现有测试

```cpp
// 原有的传统测试
TEST_CASE("Traditional test") {
    // 测试逻辑
    CHECK_EQ(SomeFunction(1, 2), 3);
}

// 迁移为 data driven 测试
std::string MigratedTest(const raftpp::datadriven::TestData& data) {
    if (data.cmd == "some_function") {
        auto a = data.GetValue("a");
        auto b = data.GetValue("b");
        if (a && b) {
            return std::to_string(SomeFunction(std::stoi(*a), std::stoi(*b)));
        }
    }
    return "";
}

TEST_CASE("Migrated data driven test") {
    raftpp::datadriven::DataDrivenTest::RunTest(
        "tests/testdata/migrated_tests", 
        MigratedTest
    );
}
```

### 2. 混合测试策略

```cpp
// 保留关键的传统测试用于快速反馈
TEST_CASE("Critical path test") {
    // 快速、关键的功能测试
    CHECK_EQ(CriticalFunction(), expected_value);
}

// 使用 data driven 测试覆盖边界情况
TEST_CASE("Comprehensive data driven tests") {
    raftpp::datadriven::DataDrivenTest::RunTest(
        "tests/testdata/comprehensive", 
        ComprehensiveTestFunction
    );
}
```

## 性能考虑

### 1. 测试数据优化

```
# ✅ 合理的测试数据量
medium_test param1=value1 param2=value2
----
expected_result

# ❌ 避免过大的测试数据
large_test param1=very_large_value_that_makes_tests_slow
----
expected_result
```

### 2. 测试函数优化

```cpp
// ✅ 高效的实现
std::string EfficientTest(const raftpp::datadriven::TestData& data) {
    // 预先检查命令类型
    if (data.cmd != "target_command") {
        return "";
    }
    
    // 使用引用避免拷贝
    const auto& args = data.cmd_args;
    
    // 高效的字符串构建
    std::string result;
    result.reserve(args.size() * 20); // 预估大小
    
    for (const auto& arg : args) {
        if (arg.HasValue()) {
            result += arg.key + "=" + arg.GetValue() + "\n";
        }
    }
    
    return result;
}
```

## 调试技巧

### 1. 使用调试输出

```cpp
std::string DebuggableTest(const raftpp::datadriven::TestData& data) {
    // 调试：输出测试数据信息
    std::cerr << "Debug: " << data.DebugString() << std::endl;
    
    // 测试逻辑
    return ProcessTest(data);
}
```

### 2. 错误定位

```cpp
std::string TestWithBetterErrors(const raftpp::datadriven::TestData& data) {
    try {
        return ProcessTest(data);
    } catch (const std::exception& e) {
        // 提供详细的错误上下文
        throw raftpp::datadriven::TestException(
            std::string("Test failed at ") + data.pos + ": " + e.what(),
            data.pos
        );
    }
}
```

这些示例展示了如何有效使用 data driven 测试框架，从基本用法到高级技巧，帮助开发者编写更清晰、更易维护的测试。