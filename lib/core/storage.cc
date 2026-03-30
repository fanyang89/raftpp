#include "raftpp/core/storage.h"

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
    GetEntriesContext context{};
    context.what = GetEntriesFor::Empty;
    context.payload.empty.can_async = can_async;
    return context;
}

Storage::~Storage() = default;

}  // namespace raftpp
