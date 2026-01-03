#include "raftpp/error.h"

#include <libassert/assert.hpp>

namespace raftpp {

bool StorageErrorOther::operator==(const StorageErrorOther&) const = default;

bool InvalidConfigError::operator==(const InvalidConfigError&) const = default;

bool ConfChangeError::operator==(const ConfChangeError&) const = default;

RaftError InvalidConfigError::ToError() const {
    return {*this};
}

RaftError::RaftError(StorageErrorCode ec) : RaftErrorInner(ec) {}

bool RaftError::operator==(const RaftError& other) const {
    return static_cast<const RaftErrorInner&>(*this) == static_cast<const RaftErrorInner&>(other);
}

}  // namespace raftpp

fmt::context::iterator fmt::formatter<raftpp::InvalidConfigError>::format(
    const raftpp::InvalidConfigError& value, const format_context& ctx
) {
    return fmt::format_to(ctx.out(), "{}", value.message);
}
