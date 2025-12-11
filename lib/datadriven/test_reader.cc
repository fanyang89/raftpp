#include "raftpp/datadriven/test_reader.h"
#include "raftpp/datadriven/exceptions.h"
#include <sstream>
#include <algorithm>

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
    
    // 处理可能的续行
    std::string full_line = Trim(current_line_);
    while (full_line.back() == '\\') {
        full_line.pop_back(); // 移除续行符
        
        if (!std::getline(content_, line)) {
            throw ParseException("Unexpected end of file after line continuation", filename_, line_number_);
        }
        ++line_number_;
        current_line_ = line;
        EmitLine(line);
        
        std::string next_line = Trim(line);
        if (!next_line.empty()) {
            full_line += " " + next_line;
        }
    }
    
    // 解析命令行
    try {
        auto [cmd, args] = LineParser::ParseLine(full_line);
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
    // 检查是否是双分隔符模式（支持空白行）
    std::string line;
    bool allow_blank_lines = false;
    
    if (std::getline(content_, line)) {
        ++line_number_;
        current_line_ = line;
        
        if (Trim(line) == "----") {
            allow_blank_lines = true;
        }
    }
    
    if (allow_blank_lines) {
        // 双分隔符模式，读取直到遇到双分隔符
        while (std::getline(content_, line)) {
            ++line_number_;
            current_line_ = line;
            
            if (Trim(line) == "----") {
                // 检查是否是结束的双分隔符
                if (std::getline(content_, line)) {
                    ++line_number_;
                    current_line_ = line;
                    
                    if (Trim(line) == "----") {
                        // 读取最后的空行
                        if (std::getline(content_, line)) {
                            ++line_number_;
                            current_line_ = line;
                            if (!Trim(line).empty()) {
                                throw ParseException("Expected blank line after double separator", filename_, line_number_);
                            }
                        }
                        break;
                    }
                }
                
                current_test_.expected += line + "\n";
                continue;
            }
            
            current_test_.expected += line + "\n";
        }
    } else {
        // 单分隔符模式，读取到第一个空行
        while (std::getline(content_, line)) {
            ++line_number_;
            current_line_ = line;
            
            if (Trim(line).empty()) {
                break;
            }
            
            current_test_.expected += line + "\n";
        }
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

bool TestDataReader::HasBlankLine(const std::string& str) {
    static const std::regex blank_line_regex(R"((?m)^[ \t]*\n)");
    return std::regex_search(str, blank_line_regex);
}

} // namespace datadriven
} // namespace raftpp