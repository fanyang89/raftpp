# Data Driven 测试数据示例

## 基本格式

测试数据文件使用以下基本格式：

```
# 注释行以 # 开头
command [arg | arg=val | arg=(val1, val2, ...)]... \
<more args> \
<more args>
----
<expected results>
<blank line>
```

## 参数格式示例

### 1. 无值参数
```
command key1 key2
----
output
```

### 2. 单值参数
```
command name=value count=42
----
output
```

### 3. 多值参数（逗号分隔）
```
command numbers=1,2,3,4,5 names=alice,bob,charlie
----
output
```

### 4. 多值参数（括号格式）
```
command items=(apple,banana,orange) scores=(95,87,92)
----
output
```

### 5. 混合格式
```
command simple multi=1,2,3 paren=(a,b,c) empty= key_only
----
output
```

## 完整测试示例

### 数学运算测试

```
# 斐波那契数列测试
fibonacci a=3 b=5 c=8
----
a=2
b=5
c=21

# 阶乘测试
factorial a=3 b=4 c=5
----
a=6
b=24
c=120

# 求和测试
sum numbers=1,2,3,4,5 values=10,20,30
----
numbers=15
values=60

# 最大值测试
max values=(1,5,3,9,2) numbers=(10,5,15,8)
----
values=9
numbers=15
```

### 字符串处理测试

```
# 字符串连接
concat str1=hello str2=world
----
hello_world

# 字符串分割
split text=hello,world,test delimiter=,
----
hello
world
test

# 字符串替换
replace text=hello world old=world new=cpp
----
hello cpp
```

### 多行输入测试

```
# 处理多行输入
process_lines \
input=first line \
second line \
third line
----
processed: first line
processed: second line
processed: third line
```

### 空白行支持的期望输出

```
# 使用双分隔符支持空白行
multiline_output
----
----
This is line 1

This is line 3 (with blank line above)

This is line 5
----
----
```

## 复杂测试场景

### Raft 算法测试示例

```
# 测试 Leader 选举
leader_election \
nodes=(1,2,3,4,5) \
term=1 \
votes=(1,2,3)
----
leader=1
term=1
votes=3

# 测试日志复制
log_replication \
leader=1 \
followers=(2,3,4,5) \
entries=(cmd1,cmd2,cmd3) \
term=2
----
success=true
replicated=4
term=2

# 测试网络分区
network_partition \
partition1=(1,2,3) \
partition2=(4,5) \
leader=1 \
term=3
----
partition1_leader=1
partition2_leader=4
term_mismatch=true
```

### 错误处理测试

```
# 无效参数测试
invalid_command bad_param=missing_quote
----
error: invalid parameter format

# 空输入测试
empty_input
----
success=true
result=empty

# 超大输入测试
large_input size=1000000
----
processed=1000000
time_ms=150
```

## 测试文件组织

### 目录结构
```
tests/testdata/
├── math_operations/
│   ├── fibonacci.txt
│   ├── factorial.txt
│   └── basic_arithmetic.txt
├── string_operations/
│   ├── concat.txt
│   ├── split.txt
│   └── replace.txt
├── raft_algorithm/
│   ├── leader_election.txt
│   ├── log_replication.txt
│   └── network_partition.txt
└── edge_cases/
    ├── empty_input.txt
    ├── invalid_format.txt
    └── large_data.txt
```

### 文件命名约定
- 使用描述性名称：`feature_name.txt`
- 避免特殊字符和空格
- 使用下划线分隔单词
- 保持名称简洁但有意义

## 最佳实践

### 1. 注释使用
- 每个测试用例前添加描述性注释
- 解释复杂测试场景的目的
- 标记边界条件和特殊情况

### 2. 参数命名
- 使用有意义的参数名
- 保持命名一致性
- 避免缩写（除非是通用缩写）

### 3. 期望输出格式
- 保持输出格式一致
- 使用键值对格式便于验证
- 包含必要的错误信息

### 4. 测试用例组织
- 按功能分组测试用例
- 从简单到复杂排列
- 包含正面和负面测试用例

## 重写模式使用

当需要更新测试数据时，可以使用重写模式：

```cpp
// 启用重写模式
DataDrivenTest::RunTest("tests/testdata/math_operations", TestFunction, true);
```

重写模式会：
1. 执行测试函数
2. 将实际输出写入测试文件
3. 替换原有的期望输出

这在使用新功能或修改测试逻辑时非常有用。

## 错误处理示例

### 解析错误
```
# 缺少分隔符
invalid_test no_separator_here
expected_output_but_no_separator
```

### 参数格式错误
```
# 不匹配的括号
bad_parens param=(unclosed
----
error: unmatched parenthesis at line 1
```

### 类型转换错误
```
# 非数字参数
numeric_test number=not_a_number
----
error: cannot convert 'not_a_number' to number at line 1
```

这些示例展示了 data driven 测试框架的灵活性和强大功能，可以适应各种测试场景。