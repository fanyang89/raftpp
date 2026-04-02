#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace raftpp {
namespace test {

/// CmdArg contains information about an argument on the directive line.
/// An argument is specified in one of the following forms:
///
/// - key         (no value)
/// - key=        (empty value)
/// - key=()      (empty value)
/// - key=a       (single value)
/// - key=a,b,c   (single value with commas - treated as one value)
/// - key=(a,b,c) (multiple values)
struct CmdArg {
    std::string key;
    std::vector<std::string> vals;

    std::string ToString() const;
};

/// TestData contains information about a datadriven testcase that was
/// parsed from the test file.
struct TestData {
    /// pos is a file:line prefix for the input test file, suitable for
    /// inclusion in logs and error messages.
    std::string pos;

    /// cmd is the first string on the directive line (up to the first whitespace).
    std::string cmd;

    /// cmd_args contains the k/v arguments to the command.
    std::vector<CmdArg> cmd_args;

    /// input is the text between the first directive line and the ---- separator.
    std::string input;

    /// expected is the value below the ---- separator.
    std::string expected;

    /// Returns true if cmd_args contains a value for the specified key.
    [[nodiscard]] bool ContainsKey(const std::string& key) const;

    /// Returns the CmdArg for the specified key, or nullopt if not found.
    [[nodiscard]] std::optional<CmdArg> GetArg(const std::string& key) const;

    /// Returns the first value for the specified key, or empty string if not found.
    [[nodiscard]] std::string GetValue(const std::string& key) const;

    /// Returns all values for the specified key.
    [[nodiscard]] std::vector<std::string> GetValues(const std::string& key) const;
};

/// TestHandler is the function type for processing test directives.
/// It receives a TestData and returns the actual output string.
using TestHandler = std::function<std::string(const TestData&)>;

/// Parse a line of datadriven input language and returns the parsed
/// command and CmdArgs.
std::pair<std::string, std::vector<CmdArg>> ParseLine(const std::string& line);

/// Run tests from a single file or all .txt files in a directory.
/// The handler function is called for each test directive.
/// If rewrite is true, the test files will be updated with the actual output.
void RunTest(const std::filesystem::path& path, TestHandler handler, bool rewrite = false);

/// Walk iterates through all .txt files in a directory and calls the
/// given function for each file path.
void Walk(
    const std::filesystem::path& path, const std::function<void(const std::filesystem::path&)>& fn
);

}  // namespace test
}  // namespace raftpp
