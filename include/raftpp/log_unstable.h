#pragma once

#include <optional>
#include <vector>

#include "raftpp/raftpp.pb.h"

namespace raftpp {

class Unstable {
public:
    explicit Unstable(uint64_t offset);
    Unstable(const std::vector<Entry>& entries, size_t entries_size, uint64_t offset, const std::optional<Snapshot>& snapshot);

    [[nodiscard]] std::optional<uint64_t> MaybeFirstIndex() const;
    [[nodiscard]] std::optional<uint64_t> MaybeLastIndex() const;
    [[nodiscard]] std::optional<uint64_t> MaybeTerm(uint64_t idx) const;
    void StableEntries(uint64_t index, uint32_t term);
    void Restore(const Snapshot& snapshot);
    void TruncateAndAppend(const std::vector<Entry>& ents);
    void TruncateAndAppend(std::span<const Entry> ents);
    void MustCheckOutOfBounds(uint64_t lo, uint64_t hi);

private:
    std::optional<Snapshot> snapshot_;
    std::vector<Entry> entries_;
    size_t entries_size_;
    uint64_t offset_;
};

}
