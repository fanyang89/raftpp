#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>
#include <spdlog/fmt/fmt.h>

struct CmdArg {
    std::string key;
    std::vector<std::string> values;
};

struct TestData {
    std::string pos;
    std::string cmd;
    std::vector<CmdArg> cmd_args;
    std::string input;
    std::string expected;

    bool ContainsKey(std::string_view key);
};

void to_json(nlohmann::json& j, const TestData& p);

template <>
struct fmt::formatter<TestData> : formatter<std::string_view> {
    static format_context::iterator format(const TestData& value, const format_context& ctx);
};

class TestDataReaderIter {
  public:
    using iterator_category = std::input_iterator_tag;
    using value_type = TestData;
    using difference_type = std::ptrdiff_t;
    using pointer = const std::string*;
    using reference = const std::string&;

    TestDataReaderIter() noexcept;  // for end()

    explicit TestDataReaderIter(const std::vector<std::string>& lines);

    TestDataReaderIter& operator++();

    TestDataReaderIter operator++(int);

    const value_type& operator*() const noexcept;

    const value_type* operator->() const noexcept;

    bool operator==(const TestDataReaderIter& other) const noexcept;

    bool operator!=(const TestDataReaderIter& other) const noexcept;

  private:
    static TestData Parse(std::string_view line);

    std::optional<std::reference_wrapper<const std::vector<std::string>>> lines_;
    size_t p_;
    TestData data_;
};

class TestDataReader {
  public:
    TestDataReader(std::string_view source_name, std::string_view content, bool rewrite);

    TestDataReaderIter begin() const { return TestDataReaderIter(lines_); }

    TestDataReaderIter end() const { return TestDataReaderIter(); }

  private:
    std::string source_name_;
    std::vector<std::string> lines_;
    std::optional<std::string> rewrite_buffer_;
};
