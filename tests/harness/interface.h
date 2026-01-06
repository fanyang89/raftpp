#pragma once

#include <memory>
#include <vector>

#include "raftpp/error.h"
#include "raftpp/raftpp.pb.h"

namespace raftpp {

class Raft;

/// A simulated Raft facade for testing.
///
/// If contained value is a valid, operations happen. If they are a null,
/// operations are a no-op.
class Interface {
  public:
    Interface() = default;
    explicit Interface(std::unique_ptr<Raft> raft);

    /// Step raft, if it exists.
    Result<void> Step(Message& m);

    /// Read messages out of raft.
    std::vector<Message> ReadMessages();

    /// Persist unstable snapshot and entries.
    void Persist();

    /// Access to underlying Raft instance
    Raft& operator*();
    Raft* operator->();
    const Raft& operator*() const;
    const Raft* operator->() const;

    /// Check if interface has a valid Raft instance
    [[nodiscard]] bool HasRaft() const;

  private:
    std::unique_ptr<Raft> raft_;
};

}  // namespace raftpp
