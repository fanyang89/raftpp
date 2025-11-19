#pragma once

#include "raftpp/raftpp.pb.h"

namespace raftpp {

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

}  // namespace raftpp
