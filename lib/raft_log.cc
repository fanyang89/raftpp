#include "raftpp/raft_log.h"

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "raftpp/util.h"

namespace raftpp {

RaftLog::RaftLog(const Config& config, std::unique_ptr<Storage> store)
    : store_(std::move(store)), unstable_(Unwrap(store_->LastIndex()) + 1) {
    const uint64_t first_index = Unwrap(store_->FirstIndex());
    const uint64_t last_index = Unwrap(store_->LastIndex());
    committed_ = first_index - 1;
    persisted_ = last_index;
    applied_ = first_index - 1;
    max_apply_unpersisted_log_limit_ = config.max_apply_unpersisted_log_limit;
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
                    "found conflict at index({}), existing_term={}, conflicting_term={}", e.index(),
                    UnwrapOr(Term(e.index()), uint64_t{0}), e.term()
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

bool RaftLog::MaybePersist(const uint64_t index, const uint64_t term) {
    uint64_t first_update_index;

    if (const auto snapshot = unstable_.snapshot()) {
        first_update_index = snapshot->get().metadata().index();
    } else {
        first_update_index = unstable_.offset();
    }

    if (index > persisted_ && index < first_update_index && store_->Term(index) == term) {
        SPDLOG_DEBUG("persisted index {}", index);
        persisted_ = index;
        return true;
    }

    return false;
}

bool RaftLog::MaybePersistSnapshot(const uint64_t index) {
    if (index <= persisted_) {
        return false;
    }

    if (index > committed_) {
        PANIC("snapshot's index {} > committed {}", index, committed_);
    }

    if (index >= unstable_.offset()) {
        PANIC("snapshot's index {} >= offset {}", index, unstable_.offset());
    }

    SPDLOG_DEBUG("snapshot persisted index {}", index);
    persisted_ = index;
    return true;
}

bool RaftLog::MaybeCommit(const uint64_t max_index, const uint64_t term) {
    if (max_index > committed_ && Term(max_index) == term) {
        CommitTo(max_index);
        return true;
    }
    return false;
}

std::optional<RaftLog::MaybeAppendResult> RaftLog::MaybeAppend(
    const uint64_t idx, const uint64_t term, const uint64_t committed, const std::vector<Entry>& entries
) {
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

Result<void> RaftLog::MustCheckOutOfBounds(uint64_t low, uint64_t high) const {
    if (low > high) {
        PANIC("invalid slice {} > {}", low, high);
    }

    const auto first_index = FirstIndex();
    if (low < first_index) {
        return RaftError(StorageErrorCode::Compacted);
    }

    const auto length = LastIndex() + 1 - first_index;
    if (low < first_index || high > first_index + length) {
        PANIC("slice[{},{}] out of bound[{},{}]", low, high, first_index, LastIndex());
    }

    return {};
}

Result<std::vector<Entry>, RaftError> RaftLog::Slice(
    uint64_t low, uint64_t high, std::optional<uint64_t> max_size, const GetEntriesContext& context
) {
    if (auto r = MustCheckOutOfBounds(low, high); !r) {
        return r.error();
    }

    if (low == high) {
        return {};
    }

    std::vector<Entry> entries;

    if (low < unstable_.offset()) {
        const auto unstable_high = std::min(high, unstable_.offset());
        if (const auto r = store_->Entries(low, unstable_high, max_size, context)) {
            entries = *r;
            if (entries.size() < unstable_high - low) {
                return entries;
            }
        } else {
            switch (r.error()) {
                case StorageErrorCode::Compacted:
                case StorageErrorCode::LogTemporarilyUnavailable:
                    return RaftError(r.error());
                case StorageErrorCode::Unavailable:
                    PANIC("entries[{}:{}] is unavailable from storage", low, unstable_high);
                    break;
                case StorageErrorCode::SnapshotOutOfDate:
                case StorageErrorCode::SnapshotTemporarilyUnavailable:
                    PANIC("unexpected error: {}", r.error());
                    break;
            }
        }
    }

    if (high > unstable_.offset()) {
        const auto offset = unstable_.offset();
        const auto unstable = unstable_.Slice(std::max(low, offset), high);
        entries.insert(entries.end(), unstable.begin(), unstable.end());
    }

    LimitSize(entries, max_size);
    return entries;
}

Result<std::vector<Entry>> RaftLog::GetEntries(
    uint64_t idx, std::optional<uint64_t> max_size, GetEntriesContext context
) {
    const auto last = LastIndex();
    if (idx > last) {
        return {};
    }
    return Slice(idx, last + 1, max_size, context);
}

void RaftLog::AppliedTo(uint64_t idx) {
    if (idx == 0) {
        return;
    }
    if (idx > committed_ || idx < applied_) {
        PANIC("applied({}) is out of range [prev_applied({}), committed({})]", idx, applied_, committed_);
    }
    AppliedToUnchecked(idx);
}

void RaftLog::AppliedToUnchecked(uint64_t idx) {
    applied_ = idx;
}

std::pair<uint64_t, std::optional<uint64_t>> RaftLog::FindConflictByTerm(uint64_t index, const uint64_t term) const {
    auto conflict_index = index;

    if (const auto last_index = LastIndex(); index > last_index) {
        SPDLOG_WARN("index({}) is out of range [0, last_index({})] in find_conflict_by_term", index, last_index);
        return {index, {}};
    }

    for (;;) {
        if (const auto t = Term(conflict_index)) {
            if (*t > term) {
                conflict_index -= 1;
            } else {
                return {conflict_index, {}};
            }
        } else {
            return {conflict_index, {}};
        }
    }
}

Result<Snapshot, StorageErrorCode> RaftLog::GetSnapshot(const uint64_t request_index, const uint64_t to) {
    if (const auto r = unstable_.snapshot()) {
        if (r->get().metadata().index() >= request_index) {
            return *r;
        }
    }
    return store_->GetSnapshot(request_index, to);
}

uint64_t RaftLog::committed() const {
    return committed_;
}

uint64_t& RaftLog::committed() {
    return committed_;
}

uint64_t RaftLog::applied() const {
    return applied_;
}

uint64_t RaftLog::persisted() const {
    return persisted_;
}

const Unstable& RaftLog::unstable() const {
    return unstable_;
}

Unstable& RaftLog::unstable() {
    return unstable_;
}

uint64_t& RaftLog::max_apply_unpersisted_log_limit() {
    return max_apply_unpersisted_log_limit_;
}

uint64_t RaftLog::max_apply_unpersisted_log_limit() const {
    return max_apply_unpersisted_log_limit_;
}

std::pair<uint64_t, uint64_t> RaftLog::CommitInfo() const {
    if (const auto r = Term(committed_)) {
        return {committed_, *r};
    } else {
        PANIC("last committed entry at {} is missing: {}", committed_, r.error());
    }
}

bool RaftLog::IsUpToDate(const uint64_t last_index, const uint64_t term) const {
    return term > LastTerm() || (term == LastTerm() && last_index >= LastIndex());
}

void RaftLog::Restore(const Snapshot& snapshot) {
    SPDLOG_INFO("restore snapshot, {}", IndexTerm(snapshot));
    const uint64_t index = snapshot.metadata().index();
    ASSERT(index >= committed_);
    if (persisted_ > committed_) {
        persisted_ = committed_;
    }
    committed_ = index;
    unstable_.Restore(snapshot);
}

uint64_t RaftLog::AppliedIndexUpperBound() const {
    return std::min(committed_, persisted_ + max_apply_unpersisted_log_limit_);
}

bool RaftLog::HasNextEntriesSince(const uint64_t since_idx) const {
    const auto offset = std::max(since_idx + 1, FirstIndex());
    const auto high = AppliedIndexUpperBound() + 1;
    return high > offset;
}

std::optional<std::vector<Entry>> RaftLog::NextEntriesSince(
    const uint64_t since_idx, const std::optional<uint64_t> max_size
) {
    const auto offset = std::max(since_idx + 1, FirstIndex());
    const auto high = AppliedIndexUpperBound() + 1;
    if (high > offset) {
        GetEntriesContext ctx;
        ctx.what = GetEntriesFor::GenReady;
        if (const auto r = Slice(offset, high, max_size, ctx)) {
            return *r;
        } else {
            PANIC("{}", r.error());
        }
    }
    return {};
}

void RaftLog::StableSnapshot(const uint64_t index) {
    unstable_.StableSnapshot(index);
}

void RaftLog::StableEntries(const uint64_t index, const uint64_t term) {
    unstable_.StableEntries(index, term);
}

}  // namespace raftpp
