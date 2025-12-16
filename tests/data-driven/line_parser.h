#pragma once

#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "test_data.h"

namespace raftpp {
namespace datadriven {

/**
 * LineParser 负责解析单行命令参数
 */
class LineParser {
  public:
    /**
     * 解析单行命令
     * @param line 要解析的行
     * @return pair<命令名, 参数列表>
     */
    static std::pair<std::string, std::vector<CmdArg>> ParseLine(const std::string& line);

  private:
    /**
     * 分割指令
     * @param line 输入行
     * @return 分割后的指令列表
     */
    static std::vector<std::string> SplitDirectives(const std::string& line);

    /**
     * 解析单个参数
     * @param arg 参数字符串
     * @return CmdArg 对象
     */
    static CmdArg ParseArgument(const std::string& arg);

    /**
     * 检查参数格式是否有效
     * @param arg 参数字符串
     * @return 是否有效
     */
    static bool IsValidArgument(const std::string& arg);

    /**
     * 去除字符串两端的空白字符
     * @param str 输入字符串
     * @return 去除空白后的字符串
     */
    static std::string Trim(const std::string& str);

    // 正则表达式模式
    static const std::regex kArgumentPattern;
};

}  // namespace datadriven
}  // namespace raftpp