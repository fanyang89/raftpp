#include "raftpp/raft_log.h"

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

namespace raftpp {

RaftLog::RaftLog(std::unique_ptr<Storage> store, const Config& cfg) :
    store_(std::move(store)),
    unstable_(Unwrap(store_->LastIndex()) + 1) {
    const uint64_t first_index = Unwrap(store_->FirstIndex());
    const uint64_t last_index = Unwrap(store_->LastIndex());
    committed_ = first_index - 1;
    persisted_ = last_index;
    applied_ = first_index - 1;
    max_apply_unpersisted_log_limit_ = cfg.max_apply_unpersisted_log_limit;
}

uint64_t RaftLog::LastTerm() const {
    if (const auto r = Term(LastIndex())) {
        return r.value();
    } else {
        PANIC("unexpected error when getting the last term: {}", r.error());
    }
}

Result<uint64_t> RaftLog::Term(const uint64_t idx) const {
    const uint64_t dummy_idx = FirstIndex() - 1;
    if (idx < dummy_idx || idx > LastIndex()) {
        return 0;
    }

    if (const auto r = unstable_.MaybeTerm(idx)) {
        return *r;
    }

    if (const auto r = store_->Term(idx)) {
        return r.value();
    } else {
        const auto err = r.error();
        switch (err) {
            case StorageErrorCode::Compacted:
            case StorageErrorCode::Unavailable:
                return RaftError(err);
            default:
                PANIC("unexpected error: {}, code: {}", magic_enum::enum_name(err), magic_enum::enum_integer(err));
        }
    }
}

uint64_t RaftLog::LastIndex() const {
    if (const auto r = unstable_.MaybeLastIndex()) {
        return *r;
    }
    return Unwrap(store_->LastIndex());
}

uint64_t RaftLog::FirstIndex() const {
    if (const auto r = unstable_.MaybeFirstIndex()) {
        return *r;
    }
    return Unwrap(store_->FirstIndex());
}

uint64_t RaftLog::FindConflict(const std::vector<Entry>& entries) const {
    for (const auto& e : entries) {
        if (!MatchTerm(e.index(), e.term())) {
            if (e.index() <= LastIndex()) {
                SPDLOG_INFO(
                    "found conflict at index({}), existing_term={}, conflicting_term={}",
                    e.index(), UnwrapOr(Term(e.index()), uint64_t{0}), e.term()
                );
            }
            return e.index();
        }
    }

    return 0;
}

bool RaftLog::MatchTerm(const uint64_t idx, const uint64_t term) const {
    if (const auto e = Term(idx)) {
        return *e == term;
    }
    return false;
}

std::optional<RaftLog::MaybeAppendResult> RaftLog::MaybeAppend(
    const uint64_t idx, const uint64_t term, uint64_t committed, const std::vector<Entry>& entries) {
    if (MatchTerm(idx, term)) {
        uint64_t conflict_idx = FindConflict(entries);
        if (conflict_idx <= committed) {
            PANIC("entry {} conflict with committed entry {}", conflict_idx, committed_);
        }
        if (conflict_idx != 0 && conflict_idx > committed) {
            const size_t start = conflict_idx - (idx + 1);
            Append(std::span{entries.begin() + start, entries.end()});

            // persisted should be decreased because entries are changed
            if (persisted_ > conflict_idx - 1) {
                persisted_ = conflict_idx - 1;
            }
        }
        const uint64_t last_new_idx = idx + entries.size();
        CommitTo(std::min(committed, last_new_idx));
        return MaybeAppendResult{conflict_idx, last_new_idx};
    }

    return std::nullopt;
}

uint64_t RaftLog::Append(std::span<const Entry> entries) {
    if (entries.empty()) {
        return LastIndex();
    }

    uint64_t after = entries.front().index() - 1;
    if (after < committed_) {
        PANIC("after {} is out of range [committed {}]", after, committed_);
    }
    unstable_.TruncateAndAppend(entries);
    return LastIndex();
}

void RaftLog::CommitTo(uint64_t to_commit) {
    if (committed_ >= to_commit) {
        return;
    }
    if (LastIndex() < to_commit) {
        PANIC("to_commit {} is out of range [last_index {}]", to_commit, LastIndex());
    }
    committed_ = to_commit;
}

uint64_t RaftLog::committed() const {
    return committed_;
}

uint64_t RaftLog::applied() const {
    return applied_;
}

}
