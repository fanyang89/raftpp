#pragma once

#include <stdexcept>
#include <string>

namespace raftpp {
namespace datadriven {

/**
 * ParseException 用于表示测试数据解析过程中的错误
 */
class ParseException : public std::runtime_error {
public:
    /**
     * 构造函数
     * @param message 错误消息
     * @param file 文件名
     * @param line 行号
     */
    ParseException(const std::string& message, const std::string& file, int line);
    
    /**
     * 获取文件名
     */
    const std::string& GetFile() const { return file_; }
    
    /**
     * 获取行号
     */
    int GetLine() const { return line_; }
    
    /**
     * 获取完整的错误信息（包含文件和行号）
     */
    std::string GetFullMessage() const;
    
private:
    std::string file_;
    int line_;
};

/**
 * TestException 用于表示测试执行过程中的错误
 */
class TestException : public std::runtime_error {
public:
    /**
     * 构造函数
     * @param message 错误消息
     * @param test_data 测试数据信息
     */
    TestException(const std::string& message, const std::string& test_data_pos);
    
    /**
     * 获取测试数据位置信息
     */
    const std::string& GetTestDataPos() const { return test_data_pos_; }
    
private:
    std::string test_data_pos_;
};

} // namespace datadriven
} // namespace raftpp