#pragma once

#include <atomic>
#include <chrono>
#include <string>

#include <nonstd/span.hpp>

#include "raftpp/core/error.h"
#include "raftpp/core/types.h"
#include "raftpp/raftor/rpc/transport.h"

namespace raftpp::raftor::rpc {

/// Transport implementation for single-process or single-node deployments.
///
/// NoopTransport satisfies the Transport interface without opening sockets. It drops outbound
/// messages and never invokes callbacks, so it should only be used when no remote peers are
/// expected.
class NoopTransport final : public Transport {
  public:
    explicit NoopTransport(TransportConfig config);
    ~NoopTransport() override;

    NoopTransport(const NoopTransport&) = delete;
    NoopTransport& operator=(const NoopTransport&) = delete;
    NoopTransport(NoopTransport&&) = delete;
    NoopTransport& operator=(NoopTransport&&) = delete;

    Result<void> Start() override;
    void Stop() override;
    void AddPeer(uint64_t id, const std::string& addr) override;
    void RemovePeer(uint64_t id) override;
    void Send(nonstd::span<const Message> messages) override;
    void SetMessageCallback(MessageCallback cb) override;
    void SetErrorCallback(ErrorCallback cb) override;
    void Poll(std::chrono::milliseconds timeout) override;
    void Run() override;

  private:
    TransportConfig config_;
    std::atomic<bool> running_{false};
    MessageCallback on_message_;
    ErrorCallback on_error_;
};

}  // namespace raftpp::raftor::rpc
