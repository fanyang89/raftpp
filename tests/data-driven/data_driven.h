#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "test_data.h"
#include "test_reader.h"

namespace raftpp {
namespace datadriven {

/**
 * DataDrivenTest 主要的测试运行器
 */
class DataDrivenTest {
  public:
    /**
     * 测试函数类型定义
     */
    using TestFunction = std::function<std::string(const TestData&)>;

    /**
     * 遍历函数类型定义
     */
    using WalkFunction = std::function<void(const std::filesystem::path&)>;

    /**
     * 运行单个文件或目录中的所有测试
     * @param path 测试文件或目录路径
     * @param func 测试函数
     * @param rewrite 是否启用重写模式
     */
    static void RunTest(const std::string& path, TestFunction func, bool rewrite = false);

    /**
     * 遍历目录中的所有文件
     * @param path 目录路径
     * @param func 处理每个文件的函数
     */
    static void WalkTests(const std::string& path, WalkFunction func);

  private:
    /**
     * 运行单个测试文件
     * @param filename 文件名
     * @param content 文件内容
     * @param func 测试函数
     * @param rewrite 是否启用重写模式
     */
    static void RunSingleTest(const std::string& filename, const std::string& content, TestFunction func, bool rewrite);

    /**
     * 运行单个测试指令
     * @param reader 测试数据读取器
     * @param func 测试函数
     */
    static void RunDirective(TestDataReader& reader, TestFunction func);

    /**
     * 检查字符串是否包含空白行
     * @param str 要检查的字符串
     * @return 是否包含空白行
     */
    static bool HasBlankLine(const std::string& str);

    /**
     * 获取目录中的所有测试文件
     * @param path 路径
     * @return 文件路径列表
     */
    static std::vector<std::string> GetTestFiles(const std::string& path);

    /**
     * 读取文件内容
     * @param filename 文件名
     * @return 文件内容
     */
    static std::string ReadFileContent(const std::string& filename);

    /**
     * 写入文件内容
     * @param filename 文件名
     * @param content 文件内容
     */
    static void WriteFileContent(const std::string& filename, const std::string& content);
};

}  // namespace datadriven
}  // namespace raftpp
