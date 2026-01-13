#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "raftpp/memory_storage.h"
#include "raftpp/raft.h"
#include "raftpp/raftpp.pb.h"

namespace raftpp {

/// A simulated Raft facade for testing.
///
/// If the contained value has a raft, operations happen. If it's nullptr,
/// operations are a no-op (used for placeholder nodes).
class Interface {
  public:
    /// Create an interface with a raft instance.
    explicit Interface(std::unique_ptr<Raft> raft);

    /// Create an interface with a raft and its storage.
    Interface(std::unique_ptr<Raft> raft, std::shared_ptr<MemoryStorage> storage);

    /// Create a no-op interface (placeholder node).
    Interface();

    Interface(Interface&&) = default;
    Interface& operator=(Interface&&) = default;

    /// Step the raft state machine.
    Result<void> Step(Message& m);

    /// Read messages out of the raft.
    std::vector<Message> ReadMessages();

    /// Persist the unstable snapshot and entries.
    void Persist();

    /// Check if this interface has a raft instance.
    bool HasRaft() const { return raft_ != nullptr; }

    /// Get the underlying raft instance.
    Raft* operator->() { return raft_.get(); }
    const Raft* operator->() const { return raft_.get(); }
    Raft& operator*() { return *raft_; }
    const Raft& operator*() const { return *raft_; }

    /// Get the underlying storage.
    std::shared_ptr<MemoryStorage> GetStorage() const { return storage_; }

    /// Accessor methods for testing
    RaftLog& raft_log() { return raft_->raft_log(); }
    const RaftLog& raft_log() const { return raft_->raft_log(); }
    StateRole state() const { return raft_->state(); }
    uint64_t term() const { return raft_->term(); }
    ProgressTracker& progress_tracker() { return raft_->progress_tracker(); }
    const ProgressTracker& progress_tracker() const { return raft_->progress_tracker(); }
    Result<ConfState> ApplyConfChange(const ConfChangeV2& cc) { return raft_->ApplyConfChange(cc); }

  private:
    std::unique_ptr<Raft> raft_;
    std::shared_ptr<MemoryStorage> storage_;
};

}  // namespace raftpp
