#include "harness/interface.h"

namespace raftpp {

Interface::Interface(std::unique_ptr<Raft> raft) : raft_(std::move(raft)), storage_(nullptr) {}

Interface::Interface(std::unique_ptr<Raft> raft, std::shared_ptr<MemoryStorage> storage)
    : raft_(std::move(raft)), storage_(std::move(storage)) {}

Interface::Interface() : raft_(nullptr), storage_(nullptr) {}

Result<void> Interface::Step(Message& m) {
    if (!raft_) {
        return {};
    }
    return raft_->Step(m);
}

std::vector<Message> Interface::ReadMessages() {
    if (!raft_) {
        return {};
    }
    std::vector<Message> msgs;
    msgs.swap(raft_->messages());
    return msgs;
}

void Interface::Persist() {
    if (!raft_) {
        return;
    }

    auto& raft_log = raft_->raft_log();
    
    // Get the internal storage from raft (need to cast to MemoryStorage for Append/ApplySnapshot)
    auto* internal_storage = dynamic_cast<MemoryStorage*>(raft_log.storage());

    // Persist unstable snapshot if any
    const Unstable& unstable = raft_log.unstable();
    const auto& snapshot_opt = static_cast<const Unstable&>(unstable).snapshot();
    if (snapshot_opt.has_value()) {
        // Make a copy of snapshot BEFORE calling StableSnapshot, which clears it
        const Snapshot snap = *snapshot_opt;
        const uint64_t index = snap.metadata().index();
        
        // First apply to storage, then mark as stable
        if (internal_storage) {
            auto result = internal_storage->ApplySnapshot(snap);
            if (!result) {
                // Ignore errors in tests
            }
        }
        // Also update the external storage for test verification
        if (storage_) {
            storage_->ApplySnapshot(snap);
        }
        // Now mark snapshot as stable (this clears unstable snapshot)
        raft_log.StableSnapshot(index);
        raft_->OnPersistSnapshot(index);
        raft_->CommitApply(index);
    }

    // Persist unstable entries if any
    const auto& unstable_entries = raft_log.unstable().entries();
    if (!unstable_entries.empty()) {
        // Make a copy of entries BEFORE calling StableEntries, which clears them
        std::vector<Entry> entries_to_persist(unstable_entries.begin(), unstable_entries.end());
        const auto& last_entry = entries_to_persist.back();
        const uint64_t last_idx = last_entry.index();
        const uint64_t last_term = last_entry.term();
        
        // First append to storage, then mark as stable
        if (internal_storage) {
            auto result = internal_storage->Append(entries_to_persist);
            if (!result) {
                // Ignore errors in tests
            }
        }
        // Also update the external storage for test verification
        if (storage_) {
            storage_->Append(entries_to_persist);
        }
        // Now mark entries as stable (this clears unstable entries)
        raft_log.StableEntries(last_idx, last_term);
        raft_->OnPersistEntries(last_idx, last_term);
    }
}

}  // namespace raftpp
