#pragma once

#include <string>
#include <vector>

namespace kvstore::cli {

struct Options {
    std::string command;
    std::string key;
    std::string value;
    std::vector<std::string> peers;
    std::string node;
    bool json_output = false;
};

Options parseArgs(int argc, char** argv);

void printHelp();

}  // namespace kvstore::cli
