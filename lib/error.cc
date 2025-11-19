#include "raftpp/error.h"

#include <libassert/assert.hpp>

namespace raftpp {

RaftError InvalidConfigError::ToError() const {
    return {*this};
}


}  // namespace raftpp

fmt::context::iterator fmt::formatter<raftpp::InvalidConfigError>::format(
    const raftpp::InvalidConfigError& value, const format_context& ctx
) {
    return fmt::format_to(ctx.out(), "{}", value.message);
}
