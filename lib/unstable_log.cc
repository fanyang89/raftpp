#include "raftpp/unstable_log.h"

#include <libassert/assert.hpp>
#include <spdlog/spdlog.h>

#include "raftpp/util.h"

namespace raftpp {

Unstable::Unstable(const uint64_t offset) : entries_size_(0), offset_(offset) {}

Unstable::Unstable(
    const std::vector<Entry>& entries, const size_t entries_size, const uint64_t offset,
    const std::optional<Snapshot>& snapshot
)
    : snapshot_(snapshot), entries_(entries), entries_size_(entries_size), offset_(offset) {}

std::optional<uint64_t> Unstable::MaybeFirstIndex() const {
    if (snapshot_) {
        return snapshot_->metadata().index() + 1;
    }
    return std::nullopt;
}

std::optional<uint64_t> Unstable::MaybeLastIndex() const {
    if (entries_.empty()) {
        if (snapshot_) {
            return snapshot_->metadata().index();
        }
        return std::nullopt;
    }
    return offset_ + entries_.size() - 1;
}

std::optional<uint64_t> Unstable::MaybeTerm(const uint64_t idx) const {
    if (idx < offset_) {
        if (!snapshot_) {
            return {};
        }

        const Snapshot& snapshot = snapshot_.value();
        const SnapshotMetadata& metadata = snapshot.metadata();
        if (idx == metadata.index()) {
            return metadata.term();
        }

        return {};
    }

    if (const auto last = MaybeLastIndex(); last.has_value()) {
        if (idx >= *last) {
            return {};
        }
        return entries_[idx - offset_].term();
    }

    return {};
}

void Unstable::StableEntries(uint64_t index, uint32_t term) {
    ASSERT(!snapshot_.has_value(), "the snapshot must be stabled before entries");

    if (entries_.empty()) {
        PANIC("unstable.slice is empty, expect its last one's index and term are {} and {}", index, term);
    }

    const auto& entry = entries_.back();
    if (entry.index() != index || entry.term() != term) {
        PANIC(
            "the last one of unstable.slice has different index {} and term {}, expect {} {}", entry.index(),
            entry.term(), index, term
        );
    }

    offset_ = entry.index() + 1;
    entries_.clear();
    entries_size_ = 0;
}

void Unstable::Restore(const Snapshot& snapshot) {
    entries_.clear();
    entries_size_ = 0;
    offset_ = snapshot.metadata().index() + 1;
    snapshot_ = snapshot;
}

void Unstable::TruncateAndAppend(const std::vector<Entry>& ents) {
    return TruncateAndAppend(std::span{ents.begin(), ents.end()});
}

void Unstable::TruncateAndAppend(std::span<const Entry> ents) {
    const uint64_t after = ents[0].index();
    if (after == offset_ + entries_.size()) {
        // after is the next index in the self.entries, append directly
    } else if (after <= offset_) {
        offset_ = after;
        entries_.clear();
        entries_size_ = 0;
    } else {
        const int64_t off = offset_;
        const int64_t diff = static_cast<int64_t>(off - after);
        MustCheckOutOfBounds(off, after);
        for (auto it = ents.begin() + diff; it != ents.end(); ++it) {
            entries_size_ -= EntryApproximateSize(*it);
        }
        entries_.erase(entries_.begin(), entries_.begin() + diff);
    }

    entries_.reserve(entries_.size() + ents.size());
    for (const auto& ent : ents) {
        entries_.emplace_back(ent);
        entries_size_ += EntryApproximateSize(ent);
    }
}

void Unstable::StableSnapshot(const uint64_t index) {
    if (const auto snapshot = snapshot_) {
        if (snapshot->metadata().index() != index) {
            PANIC("unstable.snap has different index {}, expect {}", snapshot->metadata().index(), index);
        }
        snapshot_ = {};
    } else {
        PANIC("unstable.snap is none, expect a snapshot with index {}", index);
    }
}

void Unstable::MustCheckOutOfBounds(uint64_t lo, uint64_t hi) {
    ASSERT(lo <= hi, "invalid unstable.slice {} > {}", lo, hi);

    const uint64_t upper = offset_ + entries_.size();
    ASSERT(offset_ <= lo && hi <= upper, "unstable.slice[{}, {}] out of bound[{}, {}]", lo, hi, offset_, upper);
}

std::optional<std::reference_wrapper<Snapshot>> Unstable::snapshot() {
    if (snapshot_) {
        return *snapshot_;
    }
    return {};
}

const std::optional<Snapshot>& Unstable::snapshot() const {
    return snapshot_;
}

uint64_t Unstable::offset() const {
    return offset_;
}

const std::vector<Entry>& Unstable::entries() const {
    return entries_;
}

std::span<const Entry> Unstable::Slice(const uint64_t lo, const uint64_t hi) {
    MustCheckOutOfBounds(lo, hi);
    const auto off = offset_;
    return std::span{entries_.begin() + lo - off, entries_.begin() + hi - off};
}

}  // namespace raftpp
