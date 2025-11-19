#include "raftpp/error.h"

namespace raftpp {

RaftError::RaftError(const StorageErrorCode ec) : err_(ec) {}

RaftError::RaftError(const RaftErrorCode ec) : err_(ec) {}

RaftError::RaftError(const InvalidConfigError& ec) : err_(ec) {}

RaftError::RaftError(const ConfChangeError& ec) : err_(ec) {}

RaftError InvalidConfigError::ToError() const {
    return {*this};
}

}  // namespace raftpp

fmt::context::iterator fmt::formatter<raftpp::InvalidConfigError>::format(
    const raftpp::InvalidConfigError& value, const format_context& ctx
) {
    return fmt::format_to(ctx.out(), "{}", value.message);
}
