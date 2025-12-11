#include "raftpp/datadriven/line_parser.h"
#include "raftpp/datadriven/exceptions.h"
#include <sstream>
#include <algorithm>

namespace raftpp {
namespace datadriven {

// 正则表达式模式定义
const std::regex LineParser::kArgumentPattern(
    R"(^ *[-a-zA-Z0-9/_,.]+(|=[-a-zA-Z0-9_@=+/,.]*|=\([^)]*\))( |$))"
);

std::pair<std::string, std::vector<CmdArg>> LineParser::ParseLine(const std::string& line) {
    std::string trimmed_line = Trim(line);
    
    if (trimmed_line.empty()) {
        return {"", {}};
    }
    
    auto directives = SplitDirectives(trimmed_line);
    if (directives.empty()) {
        return {"", {}};
    }
    
    std::string cmd = directives[0];
    std::vector<CmdArg> cmd_args;
    
    for (size_t i = 1; i < directives.size(); ++i) {
        cmd_args.push_back(ParseArgument(directives[i]));
    }
    
    return {cmd, cmd_args};
}

std::vector<std::string> LineParser::SplitDirectives(const std::string& line) {
    std::vector<std::string> result;
    std::string remaining = line;
    
    while (!remaining.empty()) {
        std::smatch match;
        if (std::regex_search(remaining, match, kArgumentPattern)) {
            std::string token = match[0].str();
            result.push_back(Trim(token));
            
            // 移除已匹配的部分
            size_t match_length = match[0].length();
            if (match_length >= remaining.length()) {
                break;
            }
            remaining = remaining.substr(match_length);
        } else {
            throw ParseException(
                "Cannot parse directive at column " + std::to_string(line.length() - remaining.length() + 1) + ": " + line,
                "",  // 文件名将在调用处设置
                0     // 行号将在调用处设置
            );
        }
    }
    
    return result;
}

CmdArg LineParser::ParseArgument(const std::string& arg) {
    size_t equal_pos = arg.find('=');
    
    if (equal_pos == std::string::npos) {
        // 无值参数: key
        return CmdArg(Trim(arg), {});
    }
    
    std::string key = Trim(arg.substr(0, equal_pos));
    std::string value_str = Trim(arg.substr(equal_pos + 1));
    
    if (value_str.empty()) {
        // 空值: key=
        return CmdArg(key, {});
    }
    
    if (value_str == "()") {
        // 空值: key=()
        return CmdArg(key, {});
    }
    
    if (value_str.front() == '(' && value_str.back() == ')') {
        // 多值: key=(a,b,c)
        std::string content = value_str.substr(1, value_str.length() - 2);
        std::vector<std::string> values;
        
        std::stringstream ss(content);
        std::string item;
        while (std::getline(ss, item, ',')) {
            values.push_back(Trim(item));
        }
        
        return CmdArg(key, values);
    } else {
        // 单值: key=a 或 key=a,b,c
        std::vector<std::string> values = {value_str};
        return CmdArg(key, values);
    }
}

bool LineParser::IsValidArgument(const std::string& arg) {
    return std::regex_match(arg, kArgumentPattern);
}

std::string LineParser::Trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

} // namespace datadriven
} // namespace raftpp