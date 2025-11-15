#pragma once

#include <optional>
#include <vector>

#include "types.h"

namespace raftpp {

class Unstable {
  public:
    explicit Unstable(uint64_t offset);
    Unstable(
        std::vector<Entry>&& entries, size_t entries_size, uint64_t offset,
        std::optional<Snapshot>&& snapshot
    );

    [[nodiscard]] std::optional<uint64_t> MaybeFirstIndex() const;
    [[nodiscard]] std::optional<uint64_t> MaybeLastIndex() const;
    [[nodiscard]] std::optional<uint64_t> MaybeTerm(uint64_t idx) const;
    [[nodiscard]] std::span<const Entry> Slice(uint64_t lo, uint64_t hi);
    void MustCheckOutOfBounds(uint64_t lo, uint64_t hi);
    void Restore(const Snapshot& snapshot);
    void StableEntries(uint64_t index, uint32_t term);
    void StableSnapshot(uint64_t index);
    void TruncateAndAppend(const std::vector<Entry>& ents);

    [[nodiscard]] const std::vector<Entry>& entries() const;
    [[nodiscard]] size_t entries_size() const;
    [[nodiscard]] const std::optional<Snapshot>& snapshot() const;
    [[nodiscard]] std::optional<std::reference_wrapper<Snapshot>> snapshot();
    [[nodiscard]] uint64_t offset() const;

  private:
    std::optional<Snapshot> snapshot_;
    std::vector<Entry> entries_;
    size_t entries_size_;
    uint64_t offset_;
};

}  // namespace raftpp
