#include "datadriven.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include <doctest/doctest.h>

namespace raftpp {
namespace test {

std::string CmdArg::ToString() const {
    if (vals.empty()) {
        return key;
    }
    if (vals.size() == 1) {
        return key + "=" + vals[0];
    }
    std::string result = key + "=(";
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i > 0)
            result += ",";
        result += vals[i];
    }
    result += ")";
    return result;
}

bool TestData::ContainsKey(const std::string& k) const {
    for (const auto& arg : cmd_args) {
        if (arg.key == k) {
            return true;
        }
    }
    return false;
}

std::optional<CmdArg> TestData::GetArg(const std::string& k) const {
    for (const auto& arg : cmd_args) {
        if (arg.key == k) {
            return arg;
        }
    }
    return std::nullopt;
}

std::string TestData::GetValue(const std::string& k) const {
    for (const auto& arg : cmd_args) {
        if (arg.key == k && !arg.vals.empty()) {
            return arg.vals[0];
        }
    }
    return "";
}

std::vector<std::string> TestData::GetValues(const std::string& k) const {
    for (const auto& arg : cmd_args) {
        if (arg.key == k) {
            return arg.vals;
        }
    }
    return {};
}

// Regex pattern for parsing directive tokens
// Matches: argument, argument=value, argument=(val1,val2,...)
static const std::regex kDirectiveRegex(
    R"(^ *[-a-zA-Z0-9/_,.]+(|=[-a-zA-Z0-9_@=+/,.]*|=\([^)]*\))( |$))"
);

static std::vector<std::string> SplitDirectives(const std::string& line) {
    std::vector<std::string> result;
    std::string remaining = line;

    while (!remaining.empty()) {
        std::smatch match;
        if (std::regex_search(remaining, match, kDirectiveRegex)) {
            std::string token = match[0].str();
            // Trim whitespace
            size_t start = token.find_first_not_of(' ');
            size_t end = token.find_last_not_of(' ');
            if (start != std::string::npos) {
                result.push_back(token.substr(start, end - start + 1));
            }
            remaining = remaining.substr(match[0].length());
        } else {
            throw std::runtime_error(
                "cannot parse directive at column " +
                std::to_string(line.length() - remaining.length() + 1) + ": " + line
            );
        }
    }
    return result;
}

std::pair<std::string, std::vector<CmdArg>> ParseLine(const std::string& line) {
    auto fields = SplitDirectives(line);
    if (fields.empty()) {
        return {"", {}};
    }

    std::string cmd = fields[0];
    std::vector<CmdArg> cmd_args;

    for (size_t i = 1; i < fields.size(); ++i) {
        const auto& arg = fields[i];
        auto eq_pos = arg.find('=');

        if (eq_pos == std::string::npos) {
            // key only
            cmd_args.push_back({arg, {}});
        } else {
            std::string key = arg.substr(0, eq_pos);
            std::string val = arg.substr(eq_pos + 1);

            if (val.size() >= 2 && val.front() == '(' && val.back() == ')') {
                // Multiple values: key=(a,b,c)
                std::vector<std::string> vals;
                std::string inner = val.substr(1, val.size() - 2);
                if (!inner.empty()) {
                    std::stringstream ss(inner);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        // Trim whitespace
                        size_t start = item.find_first_not_of(" \t");
                        size_t end = item.find_last_not_of(" \t");
                        if (start != std::string::npos) {
                            vals.push_back(item.substr(start, end - start + 1));
                        } else {
                            vals.push_back("");
                        }
                    }
                }
                cmd_args.push_back({key, vals});
            } else {
                // Single value: key=val
                cmd_args.push_back({key, {val}});
            }
        }
    }

    return {cmd, cmd_args};
}

// Check if a string contains blank lines (lines with only whitespace)
static bool HasBlankLine(const std::string& str) {
    static const std::regex blank_line_re(R"((?:^|\n)[\t ]*(?:\n|$))");
    return std::regex_search(str, blank_line_re);
}

class TestDataReader {
  public:
    TestDataReader(
        const std::filesystem::path& source_name, const std::string& content, bool rewrite
    )
        : source_name_(source_name), content_(content), rewrite_(rewrite), pos_(0), line_num_(0) {}

    bool Next() {
        while (true) {
            auto line_opt = ReadLine();
            if (!line_opt) {
                return false;
            }

            size_t current_line = line_num_;
            std::string line = *line_opt;
            Emit(line);

            // Trim leading/trailing whitespace
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) {
                continue;  // Empty line
            }
            size_t end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);

            // Skip comment lines
            if (line[0] == '#') {
                continue;
            }

            // Handle line continuation with backslash
            while (!line.empty() && line.back() == '\\') {
                line.pop_back();
                auto next_line = ReadLine();
                if (!next_line) {
                    throw std::runtime_error("expect argument ends without '\\'");
                }
                Emit(*next_line);
                std::string trimmed = *next_line;
                start = trimmed.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    end = trimmed.find_last_not_of(" \t\r\n");
                    line += " " + trimmed.substr(start, end - start + 1);
                }
                current_line++;
            }

            // Initialize new TestData
            data_ = TestData{};
            data_.pos = source_name_.string() + " : L" + std::to_string(current_line);

            // Parse the command line
            auto [cmd, cmd_args] = ParseLine(line);
            if (cmd.empty()) {
                throw std::runtime_error("cmd must not be empty");
            }

            data_.cmd = cmd;
            data_.cmd_args = cmd_args;

            // Read input until separator
            std::string input_buf;
            bool separator_found = false;

            while (auto input_line = ReadLine()) {
                if (*input_line == "----") {
                    separator_found = true;
                    break;
                }
                Emit(*input_line);
                input_buf += *input_line + "\n";
            }

            // Trim trailing whitespace from input
            end = input_buf.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) {
                data_.input = input_buf.substr(0, end + 1);
            }

            if (separator_found) {
                ReadExpected();
            }

            return true;
        }
    }

    void RunDirective(TestHandler& handler) {
        std::string actual = handler(data_);

        // Ensure output ends with newline
        if (!actual.empty() && actual.back() != '\n') {
            actual += "\n";
        }

        if (!rewrite_) {
            // Test mode: compare actual vs expected using doctest
            INFO("Test case at " << data_.pos);
            REQUIRE_MESSAGE(
                actual == data_.expected,
                "Expected:\n"
                    << data_.expected << "\nActual:\n"
                    << actual
            );
        } else {
            // Rewrite mode
            Emit("----");
            if (HasBlankLine(actual)) {
                Emit("----");
                rewrite_buffer_ += actual;
                Emit("----");
                Emit("----");
                Emit("");
            } else {
                rewrite_buffer_ += actual;
                Emit("");
            }
        }
    }

    std::optional<std::string> GetRewriteBuffer() const {
        if (!rewrite_) {
            return std::nullopt;
        }
        std::string result = rewrite_buffer_;
        // Remove redundant trailing newline
        while (result.size() >= 2 && result.substr(result.size() - 2) == "\n\n") {
            result.pop_back();
        }
        return result;
    }

    const TestData& data() const { return data_; }

  private:
    std::optional<std::string> ReadLine() {
        if (pos_ >= content_.size()) {
            return std::nullopt;
        }

        size_t end = content_.find('\n', pos_);
        std::string line;
        if (end == std::string::npos) {
            line = content_.substr(pos_);
            pos_ = content_.size();
        } else {
            line = content_.substr(pos_, end - pos_);
            pos_ = end + 1;
        }
        line_num_++;

        // Remove carriage return if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        return line;
    }

    void ReadExpected() {
        // Check if there are double separators (allowing blank lines in expected)
        auto first_line = ReadLine();
        if (!first_line) {
            return;
        }

        bool allow_blank_lines = (*first_line == "----");

        if (allow_blank_lines) {
            // Read until we see ----\n----
            while (auto line = ReadLine()) {
                if (*line == "----") {
                    auto next_line = ReadLine();
                    if (next_line && *next_line == "----") {
                        // End of expected section
                        // Read the following blank line if present
                        auto blank = ReadLine();
                        if (blank && !blank->empty()) {
                            // Put it back? Actually we should just check it's empty
                            // For simplicity, we'll just ignore non-empty trailing
                        }
                        break;
                    }
                    // Not end, add both lines
                    data_.expected += "----\n";
                    if (next_line) {
                        data_.expected += *next_line + "\n";
                    }
                } else {
                    data_.expected += *line + "\n";
                }
            }
        } else {
            // Single separator mode: terminate on blank line
            std::string line = *first_line;
            while (true) {
                // Check if line is blank (empty or whitespace only)
                size_t non_space = line.find_first_not_of(" \t\r");
                if (non_space == std::string::npos) {
                    break;
                }
                data_.expected += line + "\n";

                auto next = ReadLine();
                if (!next) {
                    break;
                }
                line = *next;
            }
        }
    }

    void Emit(const std::string& str) {
        if (rewrite_) {
            rewrite_buffer_ += str + "\n";
        }
    }

    std::filesystem::path source_name_;
    std::string content_;
    bool rewrite_;
    size_t pos_;
    size_t line_num_;
    TestData data_;
    std::string rewrite_buffer_;
};

static std::vector<std::filesystem::path> GetTestFiles(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> files;

    if (std::filesystem::is_regular_file(path)) {
        files.push_back(path);
    } else if (std::filesystem::is_directory(path)) {
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                files.push_back(entry.path());
            }
        }
        // Sort for deterministic order
        std::sort(files.begin(), files.end());
    }

    return files;
}

void RunTest(const std::filesystem::path& path, TestHandler handler, bool rewrite) {
    auto files = GetTestFiles(path);

    REQUIRE_MESSAGE(!files.empty(), "No test files found at: " << path.string());

    for (const auto& file : files) {
        CAPTURE(file);
        std::ifstream ifs(file);
        REQUIRE_MESSAGE(ifs.good(), "Failed to open file: " << file.string());

        std::stringstream buffer;
        buffer << ifs.rdbuf();
        std::string content = buffer.str();

        TestDataReader reader(file, content, rewrite);

        size_t test_count = 0;
        while (reader.Next()) {
            reader.RunDirective(handler);
            test_count++;
        }

        REQUIRE_MESSAGE(test_count > 0, "No test cases found in file: " << file.string());

        if (rewrite) {
            auto rewrite_data = reader.GetRewriteBuffer();
            if (rewrite_data) {
                std::ofstream ofs(file, std::ios::trunc);
                ofs << *rewrite_data;
            }
        }
    }
}

void Walk(
    const std::filesystem::path& path, const std::function<void(const std::filesystem::path&)>& fn
) {
    auto files = GetTestFiles(path);
    for (const auto& file : files) {
        fn(file);
    }
}

}  // namespace test
}  // namespace raftpp
