#include "raftpp/util.h"

namespace raftpp {

IndexTerm::IndexTerm(const uint64_t index, const uint64_t term) : index(index), term(term) {}

IndexTerm::IndexTerm(const Snapshot& snapshot) {
    const auto& m = snapshot.metadata();
    index = m.index();
    term = m.term();
}

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

bool operator==(const google::protobuf::Message& lhs, const google::protobuf::Message& rhs) {
    return google::protobuf::util::MessageDifferencer::Equals(lhs, rhs);
}

bool operator!=(const google::protobuf::Message& lhs, const google::protobuf::Message& rhs) {
    return !(lhs == rhs);
}

}  // namespace raftpp

fmt::context::iterator fmt::formatter<raftpp::IndexTerm>::format(
    const raftpp::IndexTerm& value, const format_context& ctx
) {
    const auto [i, t] = value;
    return format_to(ctx.out(), "[i={}, t={}]", i, t);
}
