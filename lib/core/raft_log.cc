#include "raftpp/core/raft_log.h"

#include <spdlog/spdlog.h>

#include "raftpp/core/util.h"

namespace raftpp {

RaftLog::RaftLog(const Config& config, const std::shared_ptr<Storage>& store)
    : store_(store), unstable_(Unwrap(store_->LastIndex()) + 1) {
    const uint64_t first_index = Unwrap(store_->FirstIndex());
    const uint64_t last_index = Unwrap(store_->LastIndex());
    committed_ = first_index - 1;
    persisted_ = last_index;
    applied_ = first_index - 1;
    max_apply_unpersisted_log_limit_ = config.max_apply_unpersisted_log_limit;
}

Result<RaftState> RaftLog::GetInitialState() {
    return store_->InitialState();
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
        if (err.Is(StorageErrorCode::Compacted) || err.Is(StorageErrorCode::Unavailable)) {
            return err;
        }
        PANIC("unexpected error: {}", err);
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
        auto e_reader = e.reader();
        if (!MatchTerm(e_reader.getIndex(), e_reader.getTerm())) {
            if (e_reader.getIndex() <= LastIndex()) {
                SPDLOG_INFO(
                    "found conflict at index({}), existing_term={}, "
                    "conflicting_term={}",
                    e_reader.getIndex(), UnwrapOr(Term(e_reader.getIndex()), uint64_t{0}),
                    e_reader.getTerm()
                );
            }
            return e_reader.getIndex();
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

    if (const auto& snapshot = unstable_.snapshot()) {
        auto meta = snapshot->get().reader().getMetadata();
        first_update_index = meta.getIndex();
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
        std::ignore = CommitTo(max_index);
        return true;
    }
    return false;
}

Result<RaftLog::MaybeAppendResult> RaftLog::MaybeAppend(
    const uint64_t idx, const uint64_t term, const uint64_t committed,
    const std::vector<Entry>& entries
) {
    if (!MatchTerm(idx, term)) {
        SPDLOG_INFO("MaybeAppend failed: idx={}, term={}, last_index={}", idx, term, LastIndex());
        return MaybeAppendResult{false, 0, 0};
    }

    uint64_t conflict_idx = FindConflict(entries);

    if (conflict_idx == 0) {
        // no conflict
    } else if (conflict_idx <= committed_) {
        return RaftError(
            FatalError{
                fmt::format("entry {} conflict with committed entry {}", conflict_idx, committed_)
            }
        );
    } else {
        const size_t start = conflict_idx - (idx + 1);
        std::vector<Entry> to_append;
        to_append.reserve(entries.size() - start);
        for (size_t i = start; i < entries.size(); ++i) {
            to_append.push_back(entries[i].clone());
        }
        std::ignore = Append(to_append);

        // persisted should be decreased because entries are changed
        persisted_ = std::min(persisted_, conflict_idx - 1);
    }

    const uint64_t last_new_idx = idx + entries.size();
    std::ignore = CommitTo(std::min(committed, last_new_idx));
    return MaybeAppendResult{true, conflict_idx, last_new_idx};
}

uint64_t RaftLog::Append(const std::vector<Entry>& entries) {
    if (entries.empty()) {
        return LastIndex();
    }

    auto first_reader = entries.front().reader();
    uint64_t after = first_reader.getIndex() - 1;
    if (after < committed_) {
        // This should not happen in normal circumstances, but we adjust for robustness
        SPDLOG_WARN(
            "after {} is out of range [committed {}], resetting committed", after, committed_
        );
        committed_ = after;
    }
    unstable_.TruncateAndAppend(entries);
    return LastIndex();
}

Result<void> RaftLog::CommitTo(uint64_t to_commit) {
    if (committed_ >= to_commit) {
        return {};
    }

    if (LastIndex() < to_commit) {
        return RaftError(
            FatalError{
                fmt::format("to_commit {} is out of range [last_index {}]", to_commit, LastIndex())
            }
        );
    }

    committed_ = to_commit;
    return {};
}

Result<void> RaftLog::MustCheckOutOfBounds(uint64_t low, uint64_t high) const {
    if (low > high) {
        return RaftError(FatalError{fmt::format("invalid slice {} > {}", low, high)});
    }

    const auto first_index = FirstIndex();
    if (low < first_index) {
        return RaftError(StorageErrorCode::Compacted);
    }

    const auto length = LastIndex() + 1 - first_index;
    if (low < first_index || high > first_index + length) {
        const auto slice_low = low;
        const auto slice_high = high;
        const auto bound_first_index = first_index;
        const auto bound_last_index = LastIndex();
        return RaftError(
            FatalError{fmt::format(
                "slice[{},{}] out of bound[{},{}]", slice_low, slice_high, bound_first_index,
                bound_last_index
            )}
        );
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
        auto r = store_->Entries(low, unstable_high, max_size, context);
        if (r) {
            entries = std::move(r).value();
            if (entries.size() < unstable_high - low) {
                return entries;
            }
        } else {
            const auto err = r.error();
            if (err.Is(StorageErrorCode::Compacted) ||
                err.Is(StorageErrorCode::LogTemporarilyUnavailable)) {
                return err;
            }

            if (err.Is(StorageErrorCode::Unavailable)) {
                PANIC("entries[{}:{}] is unavailable from storage", low, unstable_high);
            }

            PANIC("unexpected error: {}", r.error());
        }
    }

    if (high > unstable_.offset()) {
        const auto offset = unstable_.offset();
        auto unstable = unstable_.Slice(std::max(low, offset), high);
        // Clone entries from the span since Entry is move-only
        for (const auto& e : unstable) {
            entries.push_back(e.clone());
        }
    }

    LimitSize(entries, max_size);
    return entries;
}

Result<std::vector<Entry>> RaftLog::GetEntries(
    const uint64_t idx, const std::optional<uint64_t> max_size, const GetEntriesContext& context
) {
    const auto last = LastIndex();
    if (idx > last) {
        return {};
    }
    return Slice(idx, last + 1, max_size, context);
}

std::vector<Entry> RaftLog::AllEntries() {
    const auto first_index = FirstIndex();
    auto r = GetEntries(first_index, std::nullopt, GetEntriesContext::Empty(false));
    if (r) {
        return std::move(r).value();
    }
    if (r.error().Is(StorageErrorCode::Compacted)) {
        return AllEntries();
    }
    PANIC("unexpected error", r.error());
}

void RaftLog::AppliedTo(uint64_t idx) {
    if (idx == 0) {
        return;
    }
    if (idx > committed_ || idx < applied_) {
        PANIC(
            "applied({}) is out of range [prev_applied({}), committed({})]", idx, applied_,
            committed_
        );
    }
    AppliedToUnchecked(idx);
}

void RaftLog::AppliedToUnchecked(const uint64_t idx) {
    applied_ = idx;
}

std::pair<uint64_t, std::optional<uint64_t>> RaftLog::FindConflictByTerm(
    uint64_t index, const uint64_t term
) const {
    auto conflict_index = index;

    if (const auto last_index = LastIndex(); index > last_index) {
        SPDLOG_WARN(
            "index({}) is out of range [0, last_index({})] in "
            "find_conflict_by_term",
            index, last_index
        );
        return {index, {}};
    }

    for (;;) {
        if (const auto t = Term(conflict_index)) {
            if (*t > term) {
                conflict_index -= 1;
            } else {
                return {conflict_index, *t};  // Return the actual term found
            }
        } else {
            return {conflict_index, {}};
        }
    }
}

Result<Snapshot> RaftLog::GetSnapshot(const uint64_t request_index, const uint64_t to) {
    if (const auto& r = unstable_.snapshot()) {
        auto meta = r->get().reader().getMetadata();
        if (meta.getIndex() >= request_index) {
            return r->get().clone();
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

uint64_t& RaftLog::persisted() {
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

Storage* RaftLog::storage() {
    return store_.get();
}

const Storage* RaftLog::storage() const {
    return store_.get();
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

Result<void> RaftLog::Restore(const Snapshot& snapshot) {
    SPDLOG_INFO("restore snapshot, {}", IndexTerm(snapshot));
    auto meta = snapshot.reader().getMetadata();
    const uint64_t index = meta.getIndex();
    if (index < committed_) {
        return RaftError(FatalError{fmt::format("index {} < committed_ {}", index, committed_)});
    }

    if (persisted_ > committed_) {
        persisted_ = committed_;
    }
    committed_ = index;
    unstable_.Restore(snapshot);
    return {};
}

uint64_t RaftLog::AppliedIndexUpperBound() const {
    return std::min(committed_, persisted_ + max_apply_unpersisted_log_limit_);
}

bool RaftLog::HasNextEntriesSince(const uint64_t since_idx) const {
    const auto offset = std::max(since_idx + 1, FirstIndex());
    const auto high = AppliedIndexUpperBound() + 1;
    return high > offset;
}

bool RaftLog::HasNextEntries() const {
    return HasNextEntriesSince(applied_);
}

std::optional<std::vector<Entry>> RaftLog::NextEntriesSince(
    const uint64_t since_idx, const std::optional<uint64_t> max_size
) {
    const auto offset = std::max(since_idx + 1, FirstIndex());
    const auto high = AppliedIndexUpperBound() + 1;
    if (high > offset) {
        GetEntriesContext ctx;
        ctx.what = GetEntriesFor::GenReady;
        auto r = Slice(offset, high, max_size, ctx);
        if (!r) {
            PANIC("{}", r.error());
        } else {
            return std::move(r).value();
        }
    }
    return {};
}

std::optional<std::vector<Entry>> RaftLog::NextEntries(const std::optional<uint64_t> max_size) {
    return NextEntriesSince(applied_, max_size);
}

void RaftLog::StableSnapshot(const uint64_t index) {
    unstable_.StableSnapshot(index);
}

void RaftLog::StableEntries(const uint64_t index, const uint64_t term) {
    unstable_.StableEntries(index, term);
}

}  // namespace raftpp
