#pragma once

#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace raftpp {
namespace datadriven {

/**
 * CmdArg 包含命令行参数的信息
 * 
 * 支持以下格式：
 * - key         (无值)
 * - key=        (空值)
 * - key=()      (空值)
 * - key=a       (单个值)
 * - key=a,b,c   (逗号分隔的单个值)
 * - key=(a,b,c) (多个值)
 */
struct CmdArg {
    std::string key;
    std::vector<std::string> vals;

    CmdArg() = default;

    CmdArg(const std::string& k, const std::vector<std::string>& v) : key(k), vals(v) {}

    // 检查是否有值
    bool HasValue() const { return !vals.empty(); }

    // 获取单个值（如果存在）
    std::string GetValue() const {
        if (vals.empty()) {
            return "";
        }
        return vals[0];
    }

    // 获取所有值
    const std::vector<std::string>& GetValues() const { return vals; }

    // 转换为字符串表示
    std::string ToString() const {
        if (vals.empty()) {
            return key;
        } else if (vals.size() == 1) {
            return key + "=" + vals[0];
        } else {
            return key + "=(" + Join(vals, ",") + ")";
        }
    }

    bool operator==(const CmdArg& other) const { return key == other.key && vals == other.vals; }

    bool operator!=(const CmdArg& other) const { return !(*this == other); }

  private:
    static std::string Join(const std::vector<std::string>& items, const std::string& delimiter) {
        if (items.empty())
            return "";
        std::string result = items[0];
        for (size_t i = 1; i < items.size(); ++i) {
            result += delimiter + items[i];
        }
        return result;
    }
};

inline std::ostream& operator<<(std::ostream& os, const CmdArg& arg) {
    os << arg.ToString();
    return os;
}

/**
 * TestData 包含从测试文件解析出的测试用例信息
 */
struct TestData {
    /// 文件位置信息，用于错误报告
    std::string pos;

    /// 命令名
    std::string cmd;

    /// 命令参数列表
    std::vector<CmdArg> cmd_args;

    /// 输入数据（命令行和分隔符之间的内容）
    std::string input;

    /// 期望输出（分隔符之后的内容）
    std::string expected;

    TestData() = default;

    // 检查是否包含指定键的参数
    bool ContainsKey(const std::string& key) const {
        for (const auto& arg : cmd_args) {
            if (arg.key == key) {
                return true;
            }
        }
        return false;
    }

    // 获取指定键的参数
    std::optional<CmdArg> GetArg(const std::string& key) const {
        for (const auto& arg : cmd_args) {
            if (arg.key == key) {
                return arg;
            }
        }
        return std::nullopt;
    }

    // 获取指定键的值
    std::optional<std::string> GetValue(const std::string& key) const {
        auto arg = GetArg(key);
        if (arg) {
            return arg->GetValue();
        }
        return std::nullopt;
    }

    // 获取指定键的所有值
    std::optional<std::vector<std::string>> GetValues(const std::string& key) const {
        auto arg = GetArg(key);
        if (arg) {
            return arg->GetValues();
        }
        return std::nullopt;
    }

    // 调试输出
    std::string DebugString() const {
        std::string result = "TestData {\n";
        result += "  pos: " + pos + "\n";
        result += "  cmd: " + cmd + "\n";
        result += "  args: [";
        for (size_t i = 0; i < cmd_args.size(); ++i) {
            if (i > 0)
                result += ", ";
            result += cmd_args[i].ToString();
        }
        result += "]\n";
        result += "  input: \"" + input + "\"\n";
        result += "  expected: \"" + expected + "\"\n";
        result += "}";
        return result;
    }
};

}  // namespace datadriven
}  // namespace raftpp