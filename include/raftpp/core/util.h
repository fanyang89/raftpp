#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "raftpp/fmt.h"
#include "types.h"

namespace raftpp {

struct IndexTerm {
    uint64_t index;
    uint64_t term;

    IndexTerm(uint64_t index, uint64_t term);
    explicit IndexTerm(const Snapshot& snapshot);
};

size_t EntryApproximateSize(const Entry& ent);

void LimitSize(std::vector<Entry>& entries, std::optional<uint64_t> max);

bool IsContinuousEntries(const Message& message, const std::vector<Entry>& entries);

}  // namespace raftpp

template <>
struct fmt::formatter<raftpp::IndexTerm> : formatter<std::string_view> {
    static format_context::iterator format(
        const raftpp::IndexTerm& value, const format_context& ctx
    );
};
