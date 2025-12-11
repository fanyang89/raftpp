#pragma once

#include <optional>
#include <sstream>
#include <string>

#include "test_data.h"

namespace raftpp {
namespace datadriven {

/**
 * TestDataReader 负责读取和解析整个测试文件
 */
class TestDataReader {
  public:
    /**
     * 构造函数
     * @param filename 文件名
     * @param content 文件内容
     * @param rewrite 是否启用重写模式
     */
    TestDataReader(const std::string& filename, const std::string& content, bool rewrite = false);

    /**
     * 读取下一个测试用例
     * @return 是否成功读取到测试用例
     */
    bool NextTest();

    /**
     * 获取当前测试用例
     * @return 当前测试用例的引用
     */
    const TestData& GetCurrentTest() const;

    /**
     * 获取重写缓冲区内容（如果启用重写模式）
     * @return 重写缓冲区内容
     */
    std::optional<std::string> GetRewriteBuffer() const;

  private:
    /**
     * 读取期望输出部分
     */
    void ReadExpected();

    /**
     * 输出一行到重写缓冲区（如果启用重写模式）
     * @param line 要输出的行
     */
    void EmitLine(const std::string& line);

    /**
     * 去除字符串两端的空白字符
     * @param str 输入字符串
     * @return 去除空白后的字符串
     */
    std::string Trim(const std::string& str);

    /**
     * 检查字符串是否包含空白行
     * @param str 要检查的字符串
     * @return 是否包含空白行
     */
    bool HasBlankLine(const std::string& s);

    std::string filename_;
    std::istringstream content_;
    TestData current_test_;
    bool rewrite_mode_;
    std::optional<std::string> rewrite_buffer_;
    int line_number_;
    std::string current_line_;
};

}  // namespace datadriven
}  // namespace raftpp