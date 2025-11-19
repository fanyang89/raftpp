#pragma once

#include "raftpp/config.h"
#include "raftpp/log_unstable.h"
#include "raftpp/storage.h"

namespace raftpp {

class RaftLog {
  public:
    RaftLog(const Config& config, std::unique_ptr<Storage> store);

    uint64_t LastTerm() const;
    Result<uint64_t> Term(uint64_t idx) const;
    uint64_t LastIndex() const;
    uint64_t FirstIndex() const;
    uint64_t FindConflict(const std::vector<Entry>& entries) const;
    bool MatchTerm(uint64_t idx, uint64_t term) const;

    struct MaybeAppendResult {
        uint64_t conflict_index;
        uint64_t last_index;
    };

    std::optional<MaybeAppendResult> MaybeAppend(
        uint64_t idx, uint64_t term, uint64_t committed, const std::vector<Entry>& entries
    );
    uint64_t Append(std::span<const Entry> span);
    void CommitTo(uint64_t to_commit);

    uint64_t committed() const;
    uint64_t applied() const;

  private:
    std::unique_ptr<Storage> store_;
    Unstable unstable_;
    uint64_t committed_;
    uint64_t persisted_;
    uint64_t applied_;
    uint64_t max_apply_unpersisted_log_limit_;
};

}  // namespace raftpp
