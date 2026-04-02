#include "raftpp/core/util.h"

#include <limits>
#include <memory>

#include "raftpp/core/capnp_util.h"
#include "raftpp/core/types.h"

namespace raftpp {

IndexTerm::IndexTerm(const uint64_t index, const uint64_t term) : index(index), term(term) {}

IndexTerm::IndexTerm(const Snapshot& snapshot) {
    auto reader = capnp_util::reader<msg::Snapshot>(snapshot);
    auto metadata = reader.getMetadata();
    index = metadata.getIndex();
    term = metadata.getTerm();
}

size_t EntryApproximateSize(const Entry& ent) {
    // Cap'n Proto Entry message overhead (index, term, type fields)
    static constexpr size_t kEntryMessageOverhead = 12;
    auto reader = capnp_util::reader<msg::Entry>(ent);
    auto data = reader.getData();
    auto context = reader.getContext();
    return data.size() + context.size() + kEntryMessageOverhead;
}

void LimitSize(std::vector<Entry>& entries, std::optional<uint64_t> max) {
    if (entries.size() <= 1) {
        return;
    }
    if (!max.has_value() || *max == std::numeric_limits<uint64_t>::max()) {
        return;
    }

    size_t current_total_size = 0;
    size_t keep_count = 0;

    for (const auto& entry : entries) {
        const size_t entry_size = capnp_util::toBytes(entry).size();

        if (keep_count == 0) {
            current_total_size += entry_size;
            keep_count++;
            continue;
        }

        if (current_total_size + entry_size > *max) {
            break;
        }

        current_total_size += entry_size;
        keep_count++;
    }

    if (keep_count < entries.size()) {
        entries.erase(entries.begin() + static_cast<int64_t>(keep_count), entries.end());
    }
}

bool IsContinuousEntries(const Message& message, const std::vector<Entry>& entries) {
    auto msg_reader = capnp_util::reader<msg::Message>(message);
    auto msg_entries = msg_reader.getEntries();

    if (msg_entries.size() > 0 && !entries.empty()) {
        const uint64_t expected_next_idx = msg_entries[msg_entries.size() - 1].getIndex() + 1;
        return expected_next_idx == capnp_util::reader<msg::Entry>(entries.at(0)).getIndex();
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
