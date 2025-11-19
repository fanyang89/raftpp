#pragma once

#include "raftpp/config.h"
#include "raftpp/log_unstable.h"
#include "raftpp/storage.h"

namespace raftpp {

class RaftLog {
  public:
    RaftLog(const Config& config, std::unique_ptr<Storage> store);

    struct MaybeAppendResult {
        uint64_t conflict_index;
        uint64_t last_index;
    };

    uint64_t LastTerm() const;
    Result<uint64_t> Term(uint64_t idx) const;
    uint64_t LastIndex() const;
    uint64_t FirstIndex() const;
    uint64_t FindConflict(const std::vector<Entry>& entries) const;
    bool MatchTerm(uint64_t idx, uint64_t term) const;
    bool MaybePersist(uint64_t index, uint64_t term);
    bool MaybePersistSnapshot(uint64_t index);
    bool MaybeCommit(uint64_t max_index, uint64_t term);
    std::optional<MaybeAppendResult> MaybeAppend(
        uint64_t idx, uint64_t term, uint64_t committed, const std::vector<Entry>& entries
    );
    uint64_t Append(std::span<const Entry> span);
    void CommitTo(uint64_t to_commit);
    Result<std::vector<Entry>, RaftError> Slice(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size, const GetEntriesContext& context
    );
    Result<std::vector<Entry>> GetEntries(uint64_t idx, std::optional<uint64_t> max_size, GetEntriesContext context);
    void AppliedTo(uint64_t idx);
    void AppliedToUnchecked(uint64_t idx);
    std::pair<uint64_t, std::optional<uint64_t>> FindConflictByTerm(uint64_t index, uint64_t term) const;

    // return the current snapshot
    Result<Snapshot, StorageErrorCode> GetSnapshot(uint64_t request_index, uint64_t to);
    std::pair<uint64_t, uint64_t> CommitInfo() const;
    bool IsUpToDate(uint64_t last_index, uint64_t term) const;
    void Restore(const Snapshot& snapshot);

    template <typename Fn>
    Result<void> Scan(uint64_t low, uint64_t high, uint64_t page_size, GetEntriesContext ctx, Fn scanFn);

    uint64_t committed() const;
    uint64_t& committed();
    uint64_t applied() const;
    uint64_t persisted() const;
    uint64_t& max_apply_unpersisted_log_limit();
    uint64_t max_apply_unpersisted_log_limit() const;
    const Unstable& unstable() const;
    Unstable& unstable();

  private:
    Result<void> MustCheckOutOfBounds(uint64_t low, uint64_t high) const;

    std::unique_ptr<Storage> store_;
    Unstable unstable_;
    uint64_t committed_;
    uint64_t persisted_;
    uint64_t applied_;
    uint64_t max_apply_unpersisted_log_limit_;
};

template <typename Fn>
Result<void> RaftLog::Scan(uint64_t low, uint64_t high, uint64_t page_size, GetEntriesContext ctx, Fn scanFn) {
    while (low < high) {
        if (const auto ents = Slice(low, high, page_size, ctx); !ents) {
            return ents.error();
        } else {
            if (ents->empty()) {
                return RaftError(StorageErrorOther{fmt::format("got 0 entries in [{}, {})", low, high)});
            }

            low += ents->size();
            if (!scanFn(*ents)) {
                return {};
            }
        }
    }
    return {};
}

}  // namespace raftpp
