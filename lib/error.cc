#include "raftpp/error.h"

namespace raftpp {

bool StorageErrorOther::operator==(const StorageErrorOther&) const = default;

bool InvalidConfigError::operator==(const InvalidConfigError&) const = default;

RaftError ConfChangeError::ToError() const {
    return RaftError{*this};
}

bool ConfChangeError::operator==(const ConfChangeError&) const = default;

RaftError FatalError::ToError() const {
    return RaftError{*this};
}

bool FatalError::operator==(const FatalError&) const = default;

RaftError InvalidConfigError::ToError() const {
    return RaftError{*this};
}

bool RaftError::operator==(const RaftError& other) const {
    return inner_ == other.inner_;
}

}  // namespace raftpp

fmt::format_context::iterator
fmt::formatter<raftpp::InvalidConfigError>::format(
    const raftpp::InvalidConfigError& value, const format_context& ctx
) {
    return fmt::format_to(ctx.out(), "{}", value.message);
}
