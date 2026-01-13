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

    // Persist unstable snapshot if any
    const Unstable& unstable = raft_log.unstable();
    const auto& snapshot_opt = static_cast<const Unstable&>(unstable).snapshot();
    if (snapshot_opt.has_value()) {
        const Snapshot& snap = *snapshot_opt;
        const uint64_t index = snap.metadata().index();
        raft_log.StableSnapshot(index);
        if (storage_) {
            auto result = storage_->ApplySnapshot(snap);
            if (!result) {
                // Ignore errors in tests
            }
        }
        raft_->OnPersistSnapshot(index);
        raft_->CommitApply(index);
    }

    // Persist unstable entries if any
    const auto& unstable_entries = raft_log.unstable().entries();
    if (!unstable_entries.empty()) {
        const auto& last_entry = unstable_entries.back();
        const uint64_t last_idx = last_entry.index();
        const uint64_t last_term = last_entry.term();
        raft_log.StableEntries(last_idx, last_term);
        if (storage_) {
            auto result = storage_->Append(unstable_entries);
            if (!result) {
                // Ignore errors in tests
            }
        }
        raft_->OnPersistEntries(last_idx, last_term);
    }
}

}  // namespace raftpp
