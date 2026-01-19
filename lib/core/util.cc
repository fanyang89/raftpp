#include "raftpp/core/util.h"

namespace raftpp {

IndexTerm::IndexTerm(const uint64_t index, const uint64_t term) : index(index), term(term) {}

IndexTerm::IndexTerm(const Snapshot& snapshot) {
    auto reader = snapshot.reader();
    auto metadata = reader.getMetadata();
    index = metadata.getIndex();
    term = metadata.getTerm();
}

size_t EntryApproximateSize(const Entry& ent) {
    auto reader = ent.reader();
    auto data = reader.getData();
    auto context = reader.getContext();
    // TODO(fanyang) check the 12
    return data.size() + context.size() + 12;
}

bool IsContinuousEntries(const Message& message, const std::vector<Entry>& entries) {
    auto msg_reader = message.reader();
    auto msg_entries = msg_reader.getEntries();

    if (msg_entries.size() > 0 && !entries.empty()) {
        const uint64_t expected_next_idx = msg_entries[msg_entries.size() - 1].getIndex() + 1;
        return expected_next_idx == entries.at(0).reader().getIndex();
    }
    return true;
}

}  // namespace raftpp

fmt::format_context::iterator fmt::formatter<raftpp::IndexTerm>::format(
    const raftpp::IndexTerm& value, const format_context& ctx
) {
    const auto [i, t] = value;
    return format_to(ctx.out(), "[index={}, term={}]", i, t);
}
