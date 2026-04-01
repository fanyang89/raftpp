#pragma once

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>

#include "raftpp/core/error.h"
#include "raftpp/core/raft_core.h"
#include "raftpp/core/raw_node.h"  // IWYU pragma: export
#include "raftpp/raftor/proposal_tracker.h"
#include "raftpp/raftor/raftor_config.h"  // IWYU pragma: export
#include "raftpp/raftor/state_machine.h"  // IWYU pragma: export

namespace raftpp::raftor {

namespace rpc {
class Transport;
}  // namespace rpc

}  // namespace raftpp::raftor

namespace raftpp {
class Storage;
}  // namespace raftpp

namespace raftpp::raftor {

/// Node status information
struct NodeStatus {
    /// This node's ID
    uint64_t id = 0;

    /// Current role (Follower, Candidate, PreCandidate, Leader)
    StateRole role = StateRole::Follower;

    /// Current term
    uint64_t term = 0;

    /// Current leader ID (0 if unknown)
    uint64_t leader_id = 0;

    /// Commit index
    uint64_t commit_index = 0;

    /// Applied index
    uint64_t applied_index = 0;

    /// Number of pending proposals
    size_t pending_proposals = 0;
};

/// The main Raftor orchestration class
///
/// Raftor manages the complete Raft lifecycle:
/// - Event loop with ticking
/// - Ready processing in correct order
/// - Message transport
/// - State machine application
///
/// ## Usage
///
/// ```cpp
/// auto sm = std::make_unique<MyStateMachine>();
/// auto raftor = Raftor::Create(config, std::move(sm));
///
/// // Start the event loop (blocking)
/// raftor->Run();
///
/// // Or run non-blocking
/// raftor->Start();
/// raftor->Propose("data", callback);
/// raftor->Stop();
/// ```
///
/// ## Threading Model
///
/// Raftor uses a single-threaded event loop model. All Raft operations
/// happen on the event loop thread. Proposals and reads can be submitted
/// from any thread via thread-safe queues.
class Raftor {
  public:
    virtual ~Raftor() = default;

    /// Create a Raftor instance with default storage and transport
    ///
    /// This is the primary factory method. It creates:
    /// - WALStorage for persistence
    /// - CapnpTransport for networking (default)
    ///
    /// @param config The Raftor configuration
    /// @param state_machine The user's state machine implementation
    /// @return Raftor instance or error
    [[nodiscard]] static Result<std::unique_ptr<Raftor>> Create(
        const RaftorConfig& config, std::unique_ptr<StateMachine> state_machine
    );

    /// Create a Raftor instance with custom storage and transport
    ///
    /// This is primarily for testing, allowing injection of mock components.
    ///
    /// @param config The Raftor configuration
    /// @param state_machine The user's state machine implementation
    /// @param storage Custom storage implementation
    /// @param transport Custom transport implementation
    /// @return Raftor instance or error
    [[nodiscard]] static Result<std::unique_ptr<Raftor>> Create(
        const RaftorConfig& config, std::unique_ptr<StateMachine> state_machine,
        std::shared_ptr<Storage> storage, std::unique_ptr<rpc::Transport> transport
    );

    // === Lifecycle ===

    /// Start the Raftor (initialize transport, begin accepting connections)
    ///
    /// After calling Start(), the event loop is running but not blocking.
    /// Call Run() to block, or call Poll() manually for custom integration.
    ///
    /// @return void on success, or error if start fails
    [[nodiscard]] virtual Result<void> Start() = 0;

    /// Run the event loop (blocking)
    ///
    /// This blocks until Stop() is called from another thread.
    virtual void Run() = 0;

    /// Stop the Raftor gracefully
    ///
    /// This stops the event loop and closes all connections.
    /// Pending proposals and reads will be failed with a shutdown error.
    virtual void Stop() = 0;

    /// Check if the Raftor is running
    [[nodiscard]] virtual bool IsRunning() const = 0;

    // === Proposals ===

    /// Propose data to the cluster (async with callback)
    ///
    /// The callback is invoked when the proposal is committed and applied,
    /// or when it fails (e.g., not leader, dropped).
    ///
    /// Thread-safe: can be called from any thread.
    ///
    /// @param data The data to propose (will be stored in Entry.data)
    /// @param callback Called with the state machine's response or error
    virtual void Propose(std::string data, ProposalCallback callback) = 0;

    /// Propose data and wait for completion (blocking)
    ///
    /// Thread-safe: can be called from any thread.
    ///
    /// @param data The data to propose
    /// @param timeout Maximum time to wait
    /// @return The state machine's response or error
    [[nodiscard]] virtual Result<std::string> ProposeSync(
        std::string data, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}
    ) = 0;

    /// Propose data and return a future
    ///
    /// Thread-safe: can be called from any thread.
    ///
    /// @param data The data to propose
    /// @return Future that will contain the response or error
    [[nodiscard]] virtual std::future<Result<std::string>> ProposeAsync(std::string data) = 0;

    // === Reads ===

    /// Request a linearizable read (async with callback)
    ///
    /// The callback is invoked when the read index is confirmed,
    /// meaning it's safe to read from the state machine with linearizable
    /// consistency.
    ///
    /// Thread-safe: can be called from any thread.
    ///
    /// @param ctx Context string for tracking the read
    /// @param callback Called when read is safe or on error
    virtual void ReadIndex(std::string ctx, ReadIndexCallback callback) = 0;

    /// Request a linearizable read (blocking)
    ///
    /// Thread-safe: can be called from any thread.
    ///
    /// @param ctx Context string for tracking the read
    /// @param timeout Maximum time to wait
    /// @return void on success (safe to read), or error
    [[nodiscard]] virtual Result<void> ReadIndexSync(
        std::string ctx, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}
    ) = 0;

    // === Cluster Management ===

    /// Propose adding a new node to the cluster
    ///
    /// This proposes a configuration change. The node will be added
    /// once the change is committed.
    ///
    /// @param id The new node's ID
    /// @param addr The new node's address
    /// @return void on proposal submission, or error
    [[nodiscard]] virtual Result<void> AddNode(uint64_t id, const std::string& addr) = 0;

    /// Propose removing a node from the cluster
    ///
    /// This proposes a configuration change. The node will be removed
    /// once the change is committed.
    ///
    /// @param id The node ID to remove
    /// @return void on proposal submission, or error
    [[nodiscard]] virtual Result<void> RemoveNode(uint64_t id) = 0;

    /// Transfer leadership to another node
    ///
    /// This is a best-effort operation. Leadership transfer may fail
    /// if the target node is not up to date or unreachable.
    ///
    /// @param target_id The node to transfer leadership to
    virtual void TransferLeader(uint64_t target_id) = 0;

    /// Trigger an election campaign
    ///
    /// This causes this node to start an election. Primarily useful
    /// for testing or manual intervention.
    ///
    /// @return void on success, or error
    [[nodiscard]] virtual Result<void> Campaign() = 0;

    // === Status ===

    /// Get current node status
    [[nodiscard]] virtual NodeStatus GetStatus() const = 0;

    /// Check if this node is the leader
    [[nodiscard]] virtual bool IsLeader() const = 0;

    /// Get the current leader ID (0 if unknown)
    [[nodiscard]] virtual uint64_t GetLeaderId() const = 0;

    // === Advanced ===

    /// Force a snapshot to be taken
    ///
    /// This triggers the state machine to create a snapshot, which
    /// will then be persisted and used for log compaction.
    ///
    /// @return void on success, or error
    [[nodiscard]] virtual Result<void> TakeSnapshot() = 0;

    /// Get access to the underlying RawNode (for advanced use)
    ///
    /// Warning: Direct manipulation of RawNode can break Raftor's
    /// invariants. Use with caution.
    [[nodiscard]] virtual RawNode& GetRawNode() = 0;

    /// Poll the event loop once (for custom event loop integration)
    ///
    /// This processes pending events with the given timeout.
    /// Useful when integrating Raftor into an existing event loop.
    ///
    /// @param timeout Maximum time to wait for events
    virtual void Poll(std::chrono::milliseconds timeout) = 0;

    /// Process one tick manually (for testing)
    ///
    /// @return true if there was work to do
    virtual bool Tick() = 0;
};

}  // namespace raftpp::raftor
