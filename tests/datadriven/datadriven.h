#pragma once

#include <string>
#include <vector>
#include <functional>
#include <iostream>

namespace raftpp {
namespace test {

struct CmdArg {
    std::string key;
    std::vector<std::string> vals;
};

struct TestData {
    std::string pos;
    std::string cmd;
    std::vector<CmdArg> cmd_args;
    std::string expected;
};

using TestHandler = std::function<std::string(const TestData&)>;

void RunTest(const std::string& path, TestHandler handler);

} // namespace test
} // namespace raftpp
