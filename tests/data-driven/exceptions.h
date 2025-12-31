#pragma once

#include <stdexcept>
#include <string>

namespace raftpp::data_driven {

class ParseException : public std::runtime_error {
  public:
    ParseException(const std::string& message, std::string file, int line);
    [[nodiscard]] const std::string& GetFile() const;
    [[nodiscard]] int GetLine() const;
    [[nodiscard]] std::string GetFullMessage() const;

  private:
    std::string file_;
    int line_;
};

class TestException : public std::runtime_error {
  public:
    TestException(const std::string& message, std::string test_data_pos);
    [[nodiscard]] const std::string& GetTestDataPos() const;

  private:
    std::string test_data_pos_;
};

}  // namespace raftpp::data_driven
