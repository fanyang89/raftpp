#pragma once

#include <memory>

#include "raftpp/raftor/rpc/rdma_config.h"
#include "raftpp/raftor/rpc/transport.h"

namespace raftpp::raftor::rpc {

/// RDMA transport implementation using rdma-core (RC).
class RdmaTransport : public Transport {
  public:
    RdmaTransport(TransportConfig config, RdmaConfig rdma_config);
    ~RdmaTransport() override;

    RdmaTransport(const RdmaTransport&) = delete;
    RdmaTransport& operator=(const RdmaTransport&) = delete;
    RdmaTransport(RdmaTransport&&) = delete;
    RdmaTransport& operator=(RdmaTransport&&) = delete;

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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace raftpp::raftor::rpc
