#pragma once

#include <optional>
#include <spdlog/fmt/fmt.h>

#include "types.h"

namespace raftpp {

struct IndexTerm {
    uint64_t index;
    uint64_t term;

    IndexTerm(uint64_t index, uint64_t term);
    explicit IndexTerm(const Snapshot& snapshot);
};

size_t EntryApproximateSize(const Entry& ent);

// Cap'n Proto messages use serialization to determine size
template <typename T>
concept HasSerializedSize = requires(const T& t) {
    { t.serializeAsBytes() } -> std::convertible_to<std::vector<uint8_t>>;
};

template <HasSerializedSize M>
void LimitSize(std::vector<M>& entries, std::optional<uint64_t> max);

template <HasSerializedSize M>
void LimitSize(std::vector<M>& entries, const std::optional<uint64_t> max) {
    if (entries.size() <= 1) {
        return;
    }
    if (!max.has_value() || *max == std::numeric_limits<uint64_t>::max()) {
        return;
    }

    size_t current_total_size = 0;
    size_t keep_count = 0;

    for (const auto& entry : entries) {
        const size_t entry_size = entry.serializeAsBytes().size();

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
        entries.erase(entries.begin() + keep_count, entries.end());
    }
}

bool IsContinuousEntries(const Message& message, const std::vector<Entry>& entries);

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::IndexTerm> : formatter<std::string_view> {
    static format_context::iterator format(
        const raftpp::IndexTerm& value, const format_context& ctx
    );
};
