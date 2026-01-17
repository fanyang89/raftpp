#include "raftpp/wal/wal_index.h"

namespace raftpp::wal {

void WALIndex::Insert(
    uint64_t index, uint64_t segment_id, uint64_t offset, uint32_t length, uint64_t term
) {
    // If this is the first entry, set first_index
    if (entries_.empty()) {
        first_index_ = index;
    }

    // Verify the index is continuous
    uint64_t expected_index = first_index_ + entries_.size();
    if (index != expected_index) {
        // Handle case where we're inserting at a different index
        // This can happen during recovery with truncation
        if (index < first_index_) {
            // Should not happen normally
            return;
        }
        if (index > expected_index) {
            // Gap in indices - this is an error in normal operation
            // but could happen during recovery
            return;
        }
    }

    entries_.push_back(
        IndexEntry{
            .segment_id = segment_id,
            .offset = offset,
            .length = length,
            .term = term,
        }
    );
}

std::optional<IndexEntry> WALIndex::Lookup(uint64_t index) const {
    if (entries_.empty() || index < first_index_ || index > last_index()) {
        return std::nullopt;
    }
    return entries_[index - first_index_];
}

std::optional<uint64_t> WALIndex::Term(uint64_t index) const {
    auto entry = Lookup(index);
    if (!entry) {
        return std::nullopt;
    }
    return entry->term;
}

void WALIndex::TruncateFrom(uint64_t index) {
    if (entries_.empty() || index > last_index()) {
        return;
    }

    if (index <= first_index_) {
        // Truncate everything
        entries_.clear();
        return;
    }

    size_t new_size = index - first_index_;
    entries_.resize(new_size);
}

void WALIndex::TruncateBefore(uint64_t index) {
    if (entries_.empty() || index <= first_index_) {
        return;
    }

    if (index > last_index() + 1) {
        // Truncate everything
        entries_.clear();
        first_index_ = index;
        return;
    }

    size_t remove_count = index - first_index_;
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<ptrdiff_t>(remove_count));
    first_index_ = index;
}

void WALIndex::Clear() {
    entries_.clear();
    first_index_ = 1;
}

uint64_t WALIndex::last_index() const {
    if (entries_.empty()) {
        return first_index_ - 1;  // No entries yet
    }
    return first_index_ + entries_.size() - 1;
}

void WALIndex::SetFirstIndex(uint64_t first_index) {
    first_index_ = first_index;
}

}  // namespace raftpp::wal
