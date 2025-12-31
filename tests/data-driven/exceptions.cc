#include "exceptions.h"

#include <sstream>
#include <utility>

namespace raftpp::data_driven {

ParseException::ParseException(const std::string& message, std::string file, const int line)
    : std::runtime_error(message), file_(std::move(file)), line_(line) {}

const std::string& ParseException::GetFile() const {
    return file_;
}

int ParseException::GetLine() const {
    return line_;
}

std::string ParseException::GetFullMessage() const {
    std::ostringstream oss;
    oss << file_ << ":" << line_ << ": " << what();
    return oss.str();
}

TestException::TestException(const std::string& message, std::string test_data_pos)
    : std::runtime_error(message), test_data_pos_(std::move(test_data_pos)) {}

const std::string& TestException::GetTestDataPos() const {
    return test_data_pos_;
}

}  // namespace raftpp::data_driven
