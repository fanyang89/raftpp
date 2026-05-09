#include "raftpp/raftor/rpc/noop_transport.h"

#include <chrono>
#include <thread>
#include <utility>

#include "raftpp/logging.h"

namespace raftpp::raftor::rpc {

NoopTransport::NoopTransport(TransportConfig config) : config_(std::move(config)) {}

NoopTransport::~NoopTransport() {
    NoopTransport::Stop();
}

Result<void> NoopTransport::Start() {
    running_ = true;
    RAFTPP_LOG_INFO("NoopTransport started for node {}", config_.node_id);
    return {};
}

void NoopTransport::Stop() {
    if (running_.exchange(false)) {
        RAFTPP_LOG_INFO("NoopTransport stopped for node {}", config_.node_id);
    }
}

void NoopTransport::AddPeer(uint64_t id, const std::string& addr) {
    RAFTPP_LOG_WARN("NoopTransport ignoring peer {} at {}", id, addr);
}

void NoopTransport::RemovePeer(uint64_t id) {
    RAFTPP_LOG_WARN("NoopTransport ignoring peer removal for {}", id);
}

void NoopTransport::Send(nonstd::span<const Message> messages) {
    if (!messages.empty()) {
        RAFTPP_LOG_WARN("NoopTransport dropped {} outbound message(s)", messages.size());
    }
}

void NoopTransport::SetMessageCallback(MessageCallback cb) {
    on_message_ = std::move(cb);
}

void NoopTransport::SetErrorCallback(ErrorCallback cb) {
    on_error_ = std::move(cb);
}

void NoopTransport::Poll(std::chrono::milliseconds timeout) {
    if (timeout.count() > 0) {
        std::this_thread::sleep_for(timeout);
    }
}

void NoopTransport::Run() {
    while (running_) {
        Poll(std::chrono::milliseconds(100));
    }
}

}  // namespace raftpp::raftor::rpc
