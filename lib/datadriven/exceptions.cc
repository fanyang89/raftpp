#include "raftpp/datadriven/exceptions.h"
#include <sstream>

namespace raftpp {
namespace datadriven {

ParseException::ParseException(const std::string& message, const std::string& file, int line)
    : std::runtime_error(message), file_(file), line_(line) {
}

std::string ParseException::GetFullMessage() const {
    std::ostringstream oss;
    oss << file_ << ":" << line_ << ": " << what();
    return oss.str();
}

TestException::TestException(const std::string& message, const std::string& test_data_pos)
    : std::runtime_error(message), test_data_pos_(test_data_pos) {
}

} // namespace datadriven
} // namespace raftpp