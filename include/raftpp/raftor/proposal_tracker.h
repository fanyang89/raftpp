#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "raftpp/core/error.h"

namespace raftpp::raftor {

/// Callback type for proposal completion
using ProposalCallback = std::function<void(Result<std::string>)>;

/// Callback type for read index completion
using ReadIndexCallback = std::function<void(Result<void>)>;

/// Tracks pending proposals and their callbacks
///
/// When a proposal is submitted, it's tracked here until it's either:
/// - Applied successfully (callback invoked with result)
/// - Dropped (callback invoked with error)
/// - Timed out (callback invoked with timeout error)
class ProposalTracker {
  public:
    /// Register a proposal with its callback
    /// @param ctx The context string used as proposal identifier
    /// @param callback The callback to invoke when proposal completes
    /// @param timeout Time before failing with timeout (0 to disable)
    void Track(
        const std::string& ctx, ProposalCallback callback,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0}
    );

    /// Complete a proposal with success
    /// @param ctx The context string identifying the proposal
    /// @param response The response from the state machine
    void Complete(const std::string& ctx, const std::string& response);

    /// Complete a proposal with error
    /// @param ctx The context string identifying the proposal
    /// @param error The error that occurred
    void Fail(const std::string& ctx, RaftError error);

    /// Fail all pending proposals (e.g., on shutdown or leadership loss)
    /// @param error The error to report to all pending proposals
    void FailAll(RaftError error);

    /// Fail all pending reads (e.g., on shutdown or leadership loss)
    /// @param error The error to report to all pending reads
    void FailAllReads(RaftError error);

    /// Register a read index request
    /// @param ctx The context string used as read identifier
    /// @param callback The callback to invoke when read is safe
    /// @param timeout Time before failing with timeout (0 to disable)
    void TrackRead(
        const std::string& ctx, ReadIndexCallback callback,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0}
    );

    /// Complete a read index request
    /// @param ctx The context string identifying the read
    void CompleteRead(const std::string& ctx);

    /// Fail a read index request
    /// @param ctx The context string identifying the read
    /// @param error The error that occurred
    void FailRead(const std::string& ctx, RaftError error);

    /// Get the number of pending proposals
    [[nodiscard]] size_t PendingCount() const;

    /// Get the number of pending reads
    [[nodiscard]] size_t PendingReadCount() const;

    /// Expire pending proposals/reads that exceeded their timeout
    void ExpireTimeouts(std::chrono::steady_clock::time_point now);

  private:
    struct PendingProposal {
        ProposalCallback callback;
        std::chrono::steady_clock::time_point deadline;
    };

    struct PendingRead {
        ReadIndexCallback callback;
        std::chrono::steady_clock::time_point deadline;
    };

    mutable std::mutex mutex_;
    absl::flat_hash_map<std::string, PendingProposal> proposals_;
    absl::flat_hash_map<std::string, PendingRead> reads_;
};

/// Thread-safe queue for cross-thread proposal submission
///
/// Users can submit proposals from any thread, and the event loop
/// thread will consume them.
class ProposalQueue {
  public:
    /// Submit a proposal from any thread
    /// @param data The data to propose
    /// @param callback The callback to invoke when complete
    void Push(std::string data, ProposalCallback callback);

    /// Try to pop a proposal (non-blocking)
    /// @return The proposal data and callback, or nullopt if queue is empty
    [[nodiscard]] std::optional<std::pair<std::string, ProposalCallback>> TryPop();

    /// Check if the queue is empty
    [[nodiscard]] bool Empty() const;

    /// Get the number of queued proposals
    [[nodiscard]] size_t Size() const;

  private:
    mutable std::mutex mutex_;
    std::deque<std::pair<std::string, ProposalCallback>> queue_;
};

/// Thread-safe queue for cross-thread read index submission
class ReadIndexQueue {
  public:
    /// Submit a read index request from any thread
    void Push(std::string ctx, ReadIndexCallback callback);

    /// Try to pop a read request (non-blocking)
    [[nodiscard]] std::optional<std::pair<std::string, ReadIndexCallback>> TryPop();

    /// Check if the queue is empty
    [[nodiscard]] bool Empty() const;

  private:
    mutable std::mutex mutex_;
    std::deque<std::pair<std::string, ReadIndexCallback>> queue_;
};

}  // namespace raftpp::raftor
