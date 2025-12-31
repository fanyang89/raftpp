#include "test_reader.h"

#include <sstream>

#include "exceptions.h"
#include "line_parser.h"

namespace raftpp {
namespace data_driven {

TestDataReader::TestDataReader(const std::string& filename, const std::string& content, bool rewrite)
    : filename_(filename), content_(content), rewrite_mode_(rewrite), line_number_(0) {
    if (rewrite_mode_) {
        rewrite_buffer_ = std::string();
    }
}

bool TestDataReader::NextTest() {
    std::string line;
    bool found_test = false;

    // 跳过注释和空行，寻找测试开始
    while (std::getline(content_, line)) {
        ++line_number_;
        current_line_ = line;
        EmitLine(line);

        std::string trimmed = Trim(line);

        // 跳过注释行和空行
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        // 找到测试开始，开始解析
        found_test = true;
        break;
    }

    if (!found_test) {
        return false;
    }

    // 合并以反斜杠结尾的续行
    std::string command_line = current_line_;
    while (command_line.ends_with('\\')) {
        // 移除反斜杠和行尾空白
        command_line = Trim(command_line.substr(0, command_line.length() - 1));
        
        // 读取下一行
        if (!std::getline(content_, line)) {
            throw ParseException("Unexpected end of file while processing line continuation", filename_, line_number_);
        }
        ++line_number_;
        current_line_ = line;
        EmitLine(line);
        
        // 合并到命令行
        command_line += " " + Trim(line);
    }

    // 解析命令行
    try {
        auto [cmd, args] = LineParser::ParseLine(Trim(command_line));
        current_test_ = TestData();
        current_test_.pos = filename_ + ":" + std::to_string(line_number_);
        current_test_.cmd = cmd;
        current_test_.cmd_args = args;
    } catch (const ParseException& e) {
        throw ParseException(e.what(), filename_, line_number_);
    }

    // 读取输入部分
    std::string input_buffer;
    bool separator_found = false;

    while (std::getline(content_, line)) {
        ++line_number_;
        current_line_ = line;

        if (Trim(line) == "----") {
            separator_found = true;
            EmitLine(line);
            break;
        }

        EmitLine(line);
        input_buffer += line + "\n";
    }

    current_test_.input = Trim(input_buffer);

    if (separator_found) {
        // 如果输入部分为空，说明期望输出可能以 ---- 开头
        // 需要特殊处理：读取第二个 ---- 并将其包含在期望输出中
        bool empty_input = current_test_.input.empty();
        ReadExpected(empty_input);
    }

    return true;
}

const TestData& TestDataReader::GetCurrentTest() const {
    return current_test_;
}

std::optional<std::string> TestDataReader::GetRewriteBuffer() const {
    return rewrite_buffer_;
}

void TestDataReader::ReadExpected(bool include_first_separator) {
    // 读取期望输出，直到遇到空行或文件结束
    std::string line;

    // 如果需要包含第一个分隔符（当输入部分为空时）
    if (include_first_separator) {
        if (std::getline(content_, line)) {
            ++line_number_;
            current_line_ = line;
            EmitLine(line);

            std::string trimmed = Trim(line);
            if (trimmed == "----") {
                // 将分隔符添加到期望输出
                current_test_.expected += line + "\n";
            } else {
                // 不是分隔符，将行添加到期望输出并继续正常读取
                current_test_.expected += line + "\n";
                include_first_separator = false;
            }
        }
    }

    while (std::getline(content_, line)) {
        ++line_number_;
        current_line_ = line;
        EmitLine(line);

        std::string trimmed = Trim(line);

        // 如果遇到空行，停止读取
        if (trimmed.empty()) {
            break;
        }

        // 如果遇到分隔符且不是第一个分隔符，停止读取
        if (trimmed == "----" && !include_first_separator) {
            break;
        }

        // 添加到期望输出
        current_test_.expected += line + "\n";
    }
}

void TestDataReader::EmitLine(const std::string& line) {
    if (rewrite_mode_ && rewrite_buffer_) {
        *rewrite_buffer_ += line + "\n";
    }
}

std::string TestDataReader::Trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }

    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

bool TestDataReader::HasBlankLine(const std::string& s) {
    return s.size() == std::count_if(s.begin(), s.end(), [](unsigned char c) { return std::isblank(c); });
}

}  // namespace datadriven
}  // namespace raftpp
