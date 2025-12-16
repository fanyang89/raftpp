#include "test_reader.h"

#include <sstream>

#include "exceptions.h"
#include "line_parser.h"

namespace raftpp {
namespace datadriven {

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

    // 解析命令行
    try {
        auto [cmd, args] = LineParser::ParseLine(Trim(current_line_));
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
        ReadExpected();
    }

    return true;
}

const TestData& TestDataReader::GetCurrentTest() const {
    return current_test_;
}

std::optional<std::string> TestDataReader::GetRewriteBuffer() const {
    return rewrite_buffer_;
}

void TestDataReader::ReadExpected() {
    // 读取期望输出，直到遇到空行或文件结束
    std::string line;

    while (std::getline(content_, line)) {
        ++line_number_;
        current_line_ = line;
        EmitLine(line);

        std::string trimmed = Trim(line);

        // 如果遇到空行，停止读取
        if (trimmed.empty()) {
            break;
        }

        // 如果遇到分隔符，停止读取
        if (trimmed == "----") {
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
