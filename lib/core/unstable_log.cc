#include "raftpp/core/unstable_log.h"

#include <cstddef>

#include <libassert/assert.hpp>
#include <spdlog/spdlog.h>

#include "raftpp/core/util.h"

namespace raftpp {

Unstable::Unstable(const uint64_t offset) : entries_size_(0), offset_(offset) {}

Unstable::Unstable(
    std::vector<Entry>&& entries, const size_t entries_size, const uint64_t offset,
    std::optional<Snapshot>&& snapshot
)
    : snapshot_(std::move(snapshot)),
      entries_(std::move(entries)),
      entries_size_(entries_size),
      offset_(offset) {}

std::optional<uint64_t> Unstable::MaybeFirstIndex() const {
    if (snapshot_) {
        auto meta = capnp_util::reader<msg::Snapshot>(*snapshot_).getMetadata();
        return meta.getIndex() + 1;
    }
    return std::nullopt;
}

std::optional<uint64_t> Unstable::MaybeLastIndex() const {
    if (entries_.empty()) {
        if (snapshot_) {
            auto meta = capnp_util::reader<msg::Snapshot>(*snapshot_).getMetadata();
            return meta.getIndex();
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
        auto meta = capnp_util::reader<msg::Snapshot>(snapshot).getMetadata();
        if (idx == meta.getIndex()) {
            return meta.getTerm();
        }

        return {};
    }

    if (const auto last = MaybeLastIndex(); last.has_value()) {
        if (idx > *last) {
            return {};
        }
        auto entry_reader = capnp_util::reader<msg::Entry>(entries_[idx - offset_]);
        return entry_reader.getTerm();
    }

    return {};
}

void Unstable::StableEntries(uint64_t index, uint32_t term) {
    ASSERT(!snapshot_.has_value(), "the snapshot must be stabled before entries");

    if (entries_.empty()) {
        PANIC(
            "unstable.slice is empty, expect its last one's index and term are {} and {}", index,
            term
        );
    }

    const auto& entry = entries_.back();
    auto entry_reader = capnp_util::reader<msg::Entry>(entry);
    if (entry_reader.getIndex() != index || entry_reader.getTerm() != term) {
        PANIC(
            "the last one of unstable.slice has different index {} and term {}, expect {} {}",
            entry_reader.getIndex(), entry_reader.getTerm(), index, term
        );
    }

    offset_ = entry_reader.getIndex() + 1;
    entries_.clear();
    entries_size_ = 0;
}

void Unstable::Restore(const Snapshot& snapshot) {
    entries_.clear();
    entries_size_ = 0;
    auto meta = capnp_util::reader<msg::Snapshot>(snapshot).getMetadata();
    offset_ = meta.getIndex() + 1;
    snapshot_ = CloneSnapshot(snapshot);
}

void Unstable::TruncateAndAppend(const std::vector<Entry>& ents) {
    const uint64_t after = capnp_util::reader<msg::Entry>(ents.front()).getIndex();
    if (after == offset_ + entries_.size()) {
        // after is the next index in the self.entries, append directly
    } else if (after <= offset_) {
        offset_ = after;
        entries_.clear();
        entries_size_ = 0;
    } else {
        uint64_t keep_count = after - offset_;
        MustCheckOutOfBounds(offset_, after);
        for (size_t i = keep_count; i < entries_.size(); ++i) {
            entries_size_ -= EntryApproximateSize(entries_[i]);
        }
        entries_.resize(keep_count);
    }

    entries_.reserve(entries_.size() + ents.size());
    for (const auto& ent : ents) {
        entries_.emplace_back(CloneEntry(ent));
        entries_size_ += EntryApproximateSize(ent);
    }
}

void Unstable::StableSnapshot(const uint64_t index) {
    if (snapshot_.has_value()) {
        auto meta = capnp_util::reader<msg::Snapshot>(*snapshot_).getMetadata();
        if (meta.getIndex() != index) {
            PANIC("unstable.snap has different index {}, expect {}", meta.getIndex(), index);
        }
        snapshot_ = {};
    } else {
        PANIC("unstable.snap is none, expect a snapshot with index {}", index);
    }
}

void Unstable::MustCheckOutOfBounds(uint64_t lo, uint64_t hi) {
    ASSERT(lo <= hi, "invalid unstable.slice {} > {}", lo, hi);

    const uint64_t upper = offset_ + entries_.size();
    ASSERT(
        offset_ <= lo && hi <= upper, "unstable.slice[{}, {}] out of bound[{}, {}]", lo, hi,
        offset_, upper
    );
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

size_t Unstable::entries_size() const {
    return entries_size_;
}

std::span<const Entry> Unstable::Slice(const uint64_t lo, const uint64_t hi) {
    MustCheckOutOfBounds(lo, hi);
    const auto off = offset_;
    return std::span{entries_.begin() + lo - off, entries_.begin() + hi - off};
}

}  // namespace raftpp
