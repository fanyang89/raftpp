#include "raftpp/datadriven/datadriven.h"
#include "raftpp/datadriven/exceptions.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace raftpp {
namespace datadriven {

void DataDrivenTest::RunTest(const std::string& path, TestFunction func, bool rewrite) {
    auto files = GetTestFiles(path);
    
    for (const auto& file : files) {
        try {
            std::string content = ReadFileContent(file);
            RunSingleTest(file, content, func, rewrite);
        } catch (const std::exception& e) {
            FAIL("Error processing file " << file << ": " << e.what());
        }
    }
}

void DataDrivenTest::WalkTests(const std::string& path, WalkFunction func) {
    auto files = GetTestFiles(path);
    
    for (const auto& file : files) {
        std::filesystem::path file_path(file);
        func(file_path);
    }
}

void DataDrivenTest::RunSingleTest(const std::string& filename, const std::string& content, 
                               TestFunction func, bool rewrite) {
    TestDataReader reader(filename, content, rewrite);
    
    while (reader.NextTest()) {
        RunDirective(reader, func);
    }
    
    // 如果是重写模式，写入新内容
    if (rewrite) {
        auto rewrite_buffer = reader.GetRewriteBuffer();
        if (rewrite_buffer) {
            WriteFileContent(filename, *rewrite_buffer);
        }
    }
}

void DataDrivenTest::RunDirective(TestDataReader& reader, TestFunction func) {
    const TestData& test_data = reader.GetCurrentTest();
    
    try {
        std::string actual = func(test_data);
        
        // 确保输出以换行符结尾
        if (!actual.empty() && !actual.ends_with('\n')) {
            actual += '\n';
        }
        
        // 比较实际输出和期望输出
        CHECK_EQ(actual, test_data.expected);
        
    } catch (const std::exception& e) {
        FAIL("Test failed at " << test_data.pos << ": " << e.what());
    }
}

bool DataDrivenTest::HasBlankLine(const std::string& str) {
    static const std::regex blank_line_regex(R"((?m)^[ \t]*\n)");
    return std::regex_search(str, blank_line_regex);
}

std::vector<std::string> DataDrivenTest::GetTestFiles(const std::string& path) {
    std::vector<std::string> files;
    
    if (std::filesystem::is_directory(path)) {
        // 如果是目录，递归查找所有 .txt 文件
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
    } else {
        // 如果是文件，直接添加
        files.push_back(path);
    }
    
    return files;
}

std::string DataDrivenTest::ReadFileContent(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void DataDrivenTest::WriteFileContent(const std::string& filename, const std::string& content) {
    std::ofstream file(filename, std::ios::trunc);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot write to file: " + filename);
    }
    
    file << content;
    file.close();
}

} // namespace datadriven
} // namespace raftpp