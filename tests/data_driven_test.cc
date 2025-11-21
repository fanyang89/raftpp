#include "data_driven_test.h"

#include <absl/strings/str_split.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "conf_change/generated/zero.h"

bool TestData::ContainsKey(const std::string_view key) {
    for (const auto& [k, _] : cmd_args) {
        if (k == key) {
            return true;
        }
    }
    return false;
}

void to_json(nlohmann::json& j, const TestData& p) {
    j["pos"] = p.pos;
    j["cmd"] = p.cmd;

    nlohmann::json args = nlohmann::json::array();
    for (const auto& a : p.cmd_args) {
        nlohmann::json arg = nlohmann::json::object();
        arg["key"] = a.key;

        nlohmann::json values = nlohmann::json::array();
        for (const auto& v : a.values) {
            values.emplace_back(v);
        }
        arg["values"] = values;

        args.emplace_back(arg);
    }

    j["cmd_args"] = args;
    j["input"] = p.input;
    j["expected"] = p.expected;
}

fmt::context::iterator fmt::formatter<TestData>::format(const TestData& value, const format_context& ctx) {
    nlohmann::json j = value;
    return fmt::format_to(ctx.out(), "{}", j.dump());
}

TestDataReaderIter::TestDataReaderIter() noexcept : p_(0) {}

TestDataReaderIter::TestDataReaderIter(const std::vector<std::string>& lines) : lines_(lines), p_(0) {
    ++*this;
}

TestDataReaderIter& TestDataReaderIter::operator++() {
    if (p_ < lines_->get().size()) {
        data_ = Parse(lines_->get()[p_]);
        ++p_;
        return *this;
    }

    static TestDataReaderIter end;
    return end;
}

TestDataReaderIter TestDataReaderIter::operator++(int) {
    TestDataReaderIter tmp = *this;
    ++*this;
    return tmp;
}

const TestDataReaderIter::value_type& TestDataReaderIter::operator*() const noexcept {
    return data_;
}

const TestDataReaderIter::value_type* TestDataReaderIter::operator->() const noexcept {
    return &data_;
}

bool TestDataReaderIter::operator==(const TestDataReaderIter& other) const noexcept {
    if (lines_ && other.lines_ && lines_->get().size() == other.lines_->get().size() && p_ == other.p_) {
        return true;
    }

    return false;
}

bool TestDataReaderIter::operator!=(const TestDataReaderIter& other) const noexcept {
    return !(*this == other);
}

TestData TestDataReaderIter::Parse(std::string_view line) {
    TestData d{};
    return d;
}

bool SkipComment(const std::string_view line) {
    return !line.starts_with("#");
}

TestDataReader::TestDataReader(const std::string_view source_name, const std::string_view content, const bool rewrite)
    : source_name_(source_name),
      lines_(absl::StrSplit(content, "\n", SkipComment)),
      rewrite_buffer_(rewrite ? std::make_optional(std::string()) : std::nullopt) {}

TEST(TestDataReaderTest, ReadData) {
    TestDataReader reader("zero", bin2cpp::getZeroTxtFile().getBuffer(), false);
    for (auto it = reader.begin(); it != reader.end(); ++it) {
        SPDLOG_INFO("{}", nlohmann::json(*it).dump());
    }
}
