#include "raftpp/core/storage.h"

#include <libassert/assert.hpp>
#include <spdlog/fmt/fmt.h>

#include "raftpp/core/util.h"

namespace raftpp {

bool GetEntriesContext::CanAsync() const {
    if (what == GetEntriesFor::Empty) {
        return payload.empty.can_async;
    }
    if (what == GetEntriesFor::SendAppend) {
        return true;
    }
    return false;
}

GetEntriesContext GetEntriesContext::Empty(const bool can_async) {
    return GetEntriesContext{
        .what = GetEntriesFor::Empty,
        .payload = GetEntriesForPayload{.empty = {.can_async = can_async}}
    };
}

Storage::~Storage() = default;

}  // namespace raftpp
