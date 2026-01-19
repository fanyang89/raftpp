#include "raftpp/raftor/rpc/capnp_transport.h"

#include <chrono>
#include <thread>
#include <unordered_map>
#include <utility>

#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>
#include <kj/time.h>
#include <spdlog/spdlog.h>

#include "raftpp/core/capnp_message.h"
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
            owner_.EnqueueMessage(copyToOwned<raftpp::capnp::Message>(msg));
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
    kj::Own<kj::AsyncIoStream> stream;
    std::unique_ptr<::capnp::TwoPartyClient> client;
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
        try {
            RpcLoop(std::move(promise));
        } catch (const kj::Exception& e) {
            SPDLOG_ERROR("Cap'n Proto RPC thread crashed: {}", e.getDescription().cStr());
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Cap'n Proto RPC thread crashed: {}", e.what());
        } catch (...) {
            SPDLOG_ERROR("Cap'n Proto RPC thread crashed with unknown exception");
        }
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
        const auto reader = msg.reader();
        auto it = batches.find(reader.getTo());
        if (it == batches.end()) {
            it = batches.emplace(reader.getTo(), std::vector<Message>()).first;
        }
        it->second.push_back(msg.clone());
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
        auto io = kj::setupAsyncIo();
        auto addr =
            io.provider->getNetwork().parseAddress(config_.listen_addr, 0).wait(io.waitScope);
        auto listener = addr->listen();

        ::capnp::TwoPartyServer server(kj::heap<RaftTransportImpl>(*this));
        auto listen_promise = server.listen(*listener);

        std::unordered_map<uint64_t, RpcClient> clients;

        set_start({});

        while (running_ && !stopped_) {
            std::queue<OutgoingBatch> outgoing;
            {
                std::lock_guard lock(outgoing_mutex_);
                std::swap(outgoing, outgoing_queue_);
            }

            while (!outgoing.empty()) {
                auto batch = std::move(outgoing.front());
                outgoing.pop();

                std::string addr_str;
                {
                    std::lock_guard lock(peers_mutex_);
                    auto it = peers_.find(batch.peer_id);
                    if (it == peers_.end()) {
                        continue;
                    }
                    addr_str = it->second;
                }

                auto client_it = clients.find(batch.peer_id);
                if (client_it == clients.end()) {
                    auto peer_addr =
                        io.provider->getNetwork().parseAddress(addr_str, 0).wait(io.waitScope);
                    auto stream = peer_addr->connect().wait(io.waitScope);
                    auto client = std::make_unique<::capnp::TwoPartyClient>(*stream);
                    auto cap = client->bootstrap().castAs<raftpp::capnp::RaftTransport>();
                    client_it =
                        clients
                            .emplace(
                                batch.peer_id, RpcClient{std::move(stream), std::move(client), cap}
                            )
                            .first;
                }

                auto& cap = client_it->second.cap;
                try {
                    auto req = cap.sendMessagesRequest();
                    auto list = req.initMessages(batch.messages.size());
                    for (size_t i = 0; i < batch.messages.size(); ++i) {
                        list.setWithCaveats(i, batch.messages[i].reader());
                    }
                    req.send().wait(io.waitScope);
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

            // Keep the listener promise alive and drive async I/O.
            listen_promise.poll(io.waitScope);
            io.waitScope.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        clients.clear();
        listen_promise.poll(io.waitScope);
        io.waitScope.poll();
    } catch (const kj::Exception& e) {
        set_start(std::unexpected(RaftError(RpcErrorCode::BindFailed)));
        SPDLOG_ERROR("Cap'n Proto RPC loop failed: {}", e.getDescription().cStr());
        if (on_error_) {
            on_error_(0, e.getDescription().cStr());
        }
    }
}

void CapnpTransport::EnqueueMessage(Message msg) {
    std::lock_guard lock(incoming_mutex_);
    incoming_queue_.push(std::move(msg));
}

}  // namespace raftpp::raftor::rpc
