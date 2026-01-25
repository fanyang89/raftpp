#include "raftpp/raftor/rpc/capnp_transport.h"

#include <chrono>
#include <thread>
#include <unordered_map>
#include <utility>

#include <capnp/ez-rpc.h>
#include <kj/async-io.h>
#include <spdlog/spdlog.h>

#include "raftpp/core/types.h"
#include "raftpp/raftor/rpc/codec.h"

using raftpp::RaftError;
using raftpp::Result;

namespace raftpp::raftor::rpc {
namespace {

class RaftTransportImpl final : public raftpp::capnp::RaftTransport::Server {
  public:
    explicit RaftTransportImpl(CapnpTransport& owner) : owner_(owner) {}

    kj::Promise<void> sendMessages(SendMessagesContext context) override {
        auto msgs = context.getParams().getMessages();
        for (auto msg : msgs) {
            owner_.EnqueueMessage(capnp_util::clone<msg::Message>(msg));
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> sendSnapshot(SendSnapshotContext context) override {
        auto snapshot = context.getParams().getSnapshot();
        SPDLOG_WARN("Received snapshot via RPC (index={})", snapshot.getMetadata().getIndex());
        return kj::READY_NOW;
    }

  private:
    CapnpTransport& owner_;
};

struct RpcClient {
    std::unique_ptr<::capnp::EzRpcClient> client;
    raftpp::capnp::RaftTransport::Client cap;
};

}  // namespace

CapnpTransport::CapnpTransport(TransportConfig config) : config_(std::move(config)) {}

CapnpTransport::~CapnpTransport() {
    CapnpTransport::Stop();
}

Result<void> CapnpTransport::Start() {
    if (running_) {
        return {};
    }

    // Validate address format early.
    if (auto addr_result = ParseAddress(config_.listen_addr); !addr_result) {
        return std::unexpected(addr_result.error());
    }

    running_ = true;
    stopped_ = false;

    std::promise<Result<void>> start_promise;
    auto start_future = start_promise.get_future();
    rpc_thread_ = std::thread([this, promise = std::move(start_promise)]() mutable {
        RpcLoop(std::move(promise));
    });

    auto result = start_future.get();
    if (!result) {
        running_ = false;
        stopped_ = true;
        if (rpc_thread_.joinable()) {
            rpc_thread_.join();
        }
        return result;
    }

    SPDLOG_INFO("CapnpTransport started on {}", config_.listen_addr);
    return result;
}

void CapnpTransport::Stop() {
    if (!running_ || stopped_) {
        return;
    }

    stopped_ = true;
    running_ = false;

    if (rpc_thread_.joinable()) {
        rpc_thread_.join();
    }

    SPDLOG_INFO("CapnpTransport stopped");
}

void CapnpTransport::AddPeer(uint64_t id, const std::string& addr) {
    std::lock_guard lock(peers_mutex_);
    peers_[id] = addr;
}

void CapnpTransport::RemovePeer(uint64_t id) {
    std::lock_guard lock(peers_mutex_);
    peers_.erase(id);
}

void CapnpTransport::Send(std::span<const Message> messages) {
    Map<uint64_t, std::vector<Message>> batches;

    for (const auto& msg : messages) {
        const auto reader = capnp_util::reader<msg::Message>(msg);
        auto it = batches.find(reader.getTo());
        if (it == batches.end()) {
            it = batches.emplace(reader.getTo(), std::vector<Message>()).first;
        }
        it->second.push_back(CloneMessage(msg));
    }

    if (batches.empty()) {
        return;
    }

    std::lock_guard lock(outgoing_mutex_);
    for (auto& [peer_id, batch] : batches) {
        if (batch.empty()) {
            continue;
        }
        outgoing_queue_.push(OutgoingBatch{peer_id, std::move(batch)});
    }
}

void CapnpTransport::SetMessageCallback(MessageCallback cb) {
    on_message_ = std::move(cb);
}

void CapnpTransport::SetErrorCallback(ErrorCallback cb) {
    on_error_ = std::move(cb);
}

void CapnpTransport::Poll(std::chrono::milliseconds timeout) {
    {
        std::lock_guard lock(incoming_mutex_);
        while (!incoming_queue_.empty()) {
            auto msg = std::move(incoming_queue_.front());
            incoming_queue_.pop();
            if (on_message_) {
                on_message_(std::move(msg));
            }
        }
    }

    if (timeout.count() > 0) {
        std::this_thread::sleep_for(timeout);
    }
}

void CapnpTransport::Run() {
    while (running_ && !stopped_) {
        Poll(std::chrono::milliseconds(100));
    }
}

void CapnpTransport::RpcLoop(std::promise<Result<void>> start_promise) {
    bool started = false;
    auto set_start = [&](Result<void> result) {
        if (!started) {
            started = true;
            start_promise.set_value(std::move(result));
        }
    };

    try {
        auto server = std::make_unique<::capnp::EzRpcServer>(
            kj::heap<RaftTransportImpl>(*this), config_.listen_addr, 0
        );

        auto& wait_scope = server->getWaitScope();

        std::unordered_map<uint64_t, RpcClient> clients;

        set_start({});

        while (running_.load(std::memory_order_acquire) &&
               !stopped_.load(std::memory_order_acquire)) {
            std::queue<OutgoingBatch> outgoing;
            {
                std::lock_guard lock(outgoing_mutex_);
                std::swap(outgoing, outgoing_queue_);
            }

            while (!outgoing.empty()) {
                auto batch = std::move(outgoing.front());
                outgoing.pop();

                std::string addr;
                {
                    std::lock_guard lock(peers_mutex_);
                    auto it = peers_.find(batch.peer_id);
                    if (it == peers_.end()) {
                        continue;
                    }
                    addr = it->second;
                }

                auto client_it = clients.find(batch.peer_id);
                if (client_it == clients.end()) {
                    auto client = std::make_unique<::capnp::EzRpcClient>(addr, 0);
                    auto cap = client->getMain<raftpp::capnp::RaftTransport>();
                    client_it =
                        clients.emplace(batch.peer_id, RpcClient{std::move(client), cap}).first;
                }

                auto& cap = client_it->second.cap;
                try {
                    auto req = cap.sendMessagesRequest();
                    auto list = req.initMessages(batch.messages.size());
                    for (size_t i = 0; i < batch.messages.size(); ++i) {
                        list.setWithCaveats(i, capnp_util::reader<msg::Message>(batch.messages[i]));
                    }
                    req.send().wait(wait_scope);
                } catch (const kj::Exception& e) {
                    SPDLOG_WARN(
                        "RPC send to {} failed: {}", batch.peer_id, e.getDescription().cStr()
                    );
                    clients.erase(batch.peer_id);
                    if (on_error_) {
                        on_error_(batch.peer_id, e.getDescription().cStr());
                    }
                }
            }

            // Avoid driving the event loop once shutdown has been requested.
            if (!running_.load(std::memory_order_acquire) ||
                stopped_.load(std::memory_order_acquire)) {
                break;
            }

            const uint max_turns = wait_scope.poll();
            if (max_turns == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        // Best-effort teardown of internal detached tasks (EzRpcServer uses detached promises).
        // cancelAllDetached() alone can leave cancellation callbacks pending; drain a bit so that
        // tear-down doesn't run while callbacks are still firing.
        wait_scope.cancelAllDetached();
        for (int i = 0; i < 1024; ++i) {
            if (wait_scope.poll() == 0) {
                break;
            }
        }
    } catch (const kj::Exception& e) {
        set_start(std::unexpected(RaftError(RpcErrorCode::BindFailed)));
        SPDLOG_ERROR("Cap'n Proto RPC loop failed: {}", e.getDescription().cStr());
        if (on_error_) {
            on_error_(0, e.getDescription().cStr());
        }
    } catch (const std::exception& e) {
        set_start(std::unexpected(RaftError(RpcErrorCode::BindFailed)));
        SPDLOG_ERROR("Cap'n Proto RPC loop failed: {}", e.what());
    } catch (...) {
        set_start(std::unexpected(RaftError(RpcErrorCode::BindFailed)));
        SPDLOG_ERROR("Cap'n Proto RPC loop failed: unknown error");
    }
}

void CapnpTransport::EnqueueMessage(Message msg) {
    std::lock_guard lock(incoming_mutex_);
    incoming_queue_.push(std::move(msg));
}

}  // namespace raftpp::raftor::rpc
