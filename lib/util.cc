#include "raftpp/util.h"

namespace raftpp {

size_t EntryApproximateSize(const Entry& ent) {
    // TODO(fanyang) check the 12
    return ent.data().size() + ent.context().size() + 12;
}

bool IsContinuousEntries(const Message& message, const std::vector<Entry>& entries) {
    if (!message.entries().empty() && !entries.empty()) {
        const uint64_t expected_next_idx = message.entries().at(message.entries().size() - 1).index() + 1;
        return expected_next_idx == entries.at(0).index();
    }
    return true;
}

}  // namespace raftpp
