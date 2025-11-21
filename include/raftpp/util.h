#pragma once

#include <google/protobuf/message.h>
#include <google/protobuf/util/message_differencer.h>
#include <spdlog/fmt/fmt.h>

#include "raftpp/raftpp.pb.h"

namespace raftpp {

struct IndexTerm {
    uint64_t index;
    uint64_t term;

    IndexTerm(uint64_t index, uint64_t term);
    explicit IndexTerm(const Snapshot& snapshot);
};

size_t EntryApproximateSize(const Entry& ent);

template <typename T>
concept HasByteSizeLong = requires(const T& t) {
    { t.ByteSizeLong() } -> std::convertible_to<std::size_t>;
};

template <HasByteSizeLong M>
void LimitSize(std::vector<M>& entries, std::optional<uint64_t> max);

template <HasByteSizeLong M>
void LimitSize(std::vector<M>& entries, const std::optional<uint64_t> max) {
    if (entries.size() <= 1) {
        return;
    }
    if (!max.has_value()) {
        return;
    }
    if (*max == std::numeric_limits<uint64_t>::max()) {
        return;
    }

    size_t size = 0;
    size_t limit = 0;
    for (auto& entry : entries) {
        if (size >= *max) {
            break;
        }
        size += entry.ByteSizeLong();
        ++limit;
    }

    entries.erase(entries.begin(), entries.begin() + limit);
}

bool IsContinuousEntries(const Message& message, const std::vector<Entry>& entries);

bool operator==(const google::protobuf::Message& lhs, const google::protobuf::Message& rhs);
bool operator!=(const google::protobuf::Message& lhs, const google::protobuf::Message& rhs);

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::IndexTerm> : formatter<std::string_view> {
    static format_context::iterator format(const raftpp::IndexTerm& value, const format_context& ctx);
};
