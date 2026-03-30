#pragma once

#include <atomic>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "raftpp/core/primitives.h"
#include "raftpp/raftor/rpc/transport.h"

namespace raftpp::raftor::rpc {

/// RPC transport implementation using Cap'n Proto RPC.
class CapnpTransport : public Transport {
  public:
    explicit CapnpTransport(TransportConfig config);
    ~CapnpTransport() override;

    CapnpTransport(const CapnpTransport&) = delete;
    CapnpTransport& operator=(const CapnpTransport&) = delete;
    CapnpTransport(CapnpTransport&&) = delete;
    CapnpTransport& operator=(CapnpTransport&&) = delete;

    Result<void> Start() override;
    void Stop() override;
    void AddPeer(uint64_t id, const std::string& addr) override;
    void RemovePeer(uint64_t id) override;
    void Send(nonstd::span<const Message> messages) override;
    void SetMessageCallback(MessageCallback cb) override;
    void SetErrorCallback(ErrorCallback cb) override;
    void Poll(std::chrono::milliseconds timeout) override;
    void Run() override;
    void EnqueueMessage(Message msg);

  private:
    struct OutgoingBatch {
        uint64_t peer_id = 0;
        std::vector<Message> messages;
    };

    struct ErrorEvent {
        uint64_t peer_id = 0;
        std::string error;
    };

    void RpcLoop(std::promise<Result<void>> start_promise);
    void EnqueueError(uint64_t peer_id, std::string error);

    TransportConfig config_;

    std::mutex peers_mutex_;
    Map<uint64_t, std::string> peers_;

    std::mutex outgoing_mutex_;
    std::queue<OutgoingBatch> outgoing_queue_;

    std::mutex incoming_mutex_;
    std::queue<Message> incoming_queue_;

    std::mutex error_mutex_;
    std::queue<ErrorEvent> error_queue_;

    std::mutex callback_mutex_;
    MessageCallback on_message_;
    ErrorCallback on_error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false};
    std::thread rpc_thread_;
};

}  // namespace raftpp::raftor::rpc
