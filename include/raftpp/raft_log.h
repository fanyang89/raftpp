#pragma once

#include "raftpp/raft_config.h"
#include "raftpp/storage.h"
#include "raftpp/unstable_log.h"

namespace raftpp {

class RaftLog {
  public:
    RaftLog(const Config& config, const std::shared_ptr<Storage>& store);

    struct MaybeAppendResult {
        bool term_matched = false;
        uint64_t conflict_index = 0;
        uint64_t last_index = 0;
    };

    [[nodiscard]] Result<Snapshot> GetSnapshot(
        uint64_t request_index, uint64_t to
    );  // return the current snapshot
    [[nodiscard]] Result<RaftState> GetInitialState();
    [[nodiscard]] Result<std::vector<Entry>> Slice(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size,
        const GetEntriesContext& context
    );
    [[nodiscard]] Result<std::vector<Entry>> GetEntries(
        uint64_t idx, std::optional<uint64_t> max_size, const GetEntriesContext& context
    );
    [[nodiscard]] std::vector<Entry> AllEntries();
    [[nodiscard]] Result<uint64_t> Term(uint64_t idx) const;
    [[nodiscard]] bool HasNextEntriesSince(uint64_t since_idx) const;
    [[nodiscard]] bool HasNextEntries() const;
    [[nodiscard]] bool IsUpToDate(uint64_t last_index, uint64_t term) const;
    [[nodiscard]] bool MatchTerm(uint64_t idx, uint64_t term) const;
    [[nodiscard]] bool MaybeCommit(uint64_t max_index, uint64_t term);
    [[nodiscard]] bool MaybePersist(uint64_t index, uint64_t term);
    [[nodiscard]] bool MaybePersistSnapshot(uint64_t index);
    [[nodiscard]] Result<MaybeAppendResult> MaybeAppend(
        uint64_t idx, uint64_t term, uint64_t committed, const std::vector<Entry>& entries
    );
    [[nodiscard]] std::optional<std::vector<Entry>> NextEntriesSince(
        uint64_t since_idx, std::optional<uint64_t> max_size
    );
    [[nodiscard]] std::optional<std::vector<Entry>> NextEntries(std::optional<uint64_t> max_size);
    [[nodiscard]] std::pair<uint64_t, std::optional<uint64_t>> FindConflictByTerm(
        uint64_t index, uint64_t term
    ) const;
    [[nodiscard]] std::pair<uint64_t, uint64_t> CommitInfo() const;

    template <typename Fn>
    [[nodiscard]] Result<void> Scan(
        uint64_t low, uint64_t high, uint64_t page_size, GetEntriesContext ctx, Fn scanFn
    );

    [[nodiscard]] uint64_t Append(const std::vector<Entry>& entries);
    [[nodiscard]] uint64_t AppliedIndexUpperBound() const;
    [[nodiscard]] uint64_t FindConflict(const std::vector<Entry>& entries) const;
    [[nodiscard]] uint64_t FirstIndex() const;
    [[nodiscard]] uint64_t LastIndex() const;
    [[nodiscard]] uint64_t LastTerm() const;
    void AppliedTo(uint64_t idx);
    void AppliedToUnchecked(uint64_t idx);
    [[nodiscard]] Result<void> CommitTo(uint64_t to_commit);
    [[nodiscard]] Result<void> Restore(const Snapshot& snapshot);
    void StableEntries(uint64_t index, uint64_t term);
    void StableSnapshot(uint64_t index);

    [[nodiscard]] Unstable& unstable();
    [[nodiscard]] const Unstable& unstable() const;
    [[nodiscard]] uint64_t applied() const;
    [[nodiscard]] uint64_t committed() const;
    [[nodiscard]] uint64_t& committed();
    [[nodiscard]] uint64_t max_apply_unpersisted_log_limit() const;
    [[nodiscard]] uint64_t& max_apply_unpersisted_log_limit();
    [[nodiscard]] uint64_t persisted() const;
    [[nodiscard]] uint64_t& persisted();
    [[nodiscard]] Storage* storage();
    [[nodiscard]] const Storage* storage() const;

  protected:
    [[nodiscard]] Result<void> MustCheckOutOfBounds(uint64_t low, uint64_t high) const;

  private:
    std::shared_ptr<Storage> store_;
    Unstable unstable_;
    uint64_t committed_;
    uint64_t persisted_;
    uint64_t applied_;
    uint64_t max_apply_unpersisted_log_limit_;
};

template <typename Fn>
Result<void> RaftLog::Scan(
    uint64_t low, uint64_t high, uint64_t page_size, GetEntriesContext ctx, Fn scanFn
) {
    while (low < high) {
        if (const auto ents = Slice(low, high, page_size, ctx); !ents) {
            return ents.error();
        } else {
            if (ents->empty()) {
                return RaftError(
                    StorageErrorOther{fmt::format("got 0 entries in [{}, {})", low, high)}
                );
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
