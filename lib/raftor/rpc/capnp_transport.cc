#include "raftpp/raftor/rpc/capnp_transport.h"

#include <string.h>
#include <sys/types.h>

#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>

#include <capnp/capability.h>
#include <capnp/ez-rpc.h>
#include <kj/async-prelude.h>
#include <kj/async.h>
#include <kj/common.h>
#include <kj/exception.h>
#include <kj/memory.h>
#include <kj/string.h>
#include <nonstd/expected.hpp>
#include <nonstd/span.hpp>
#include <opentelemetry/trace/span.h>

#include "raftpp.capnp.h"
#include "raftpp/core/capnp_util.h"
#include "raftpp/core/types.h"
#include "raftpp/fmt.h"
#include "raftpp/logging.h"
#include "raftpp/raftor/rpc/codec.h"
#include "raftpp/raftor/telemetry.h"
#include "transport.h"

using raftpp::RaftError;
using raftpp::Result;

namespace raftpp::raftor::rpc {
namespace {

constexpr size_t kMaxPendingIncomingMessages = 4096;
constexpr size_t kMaxPendingOutgoingBatches = 1024;
constexpr size_t kMaxPendingErrorEvents = 1024;
constexpr size_t kMaxInFlightSendTasks = 4096;

class RaftTransportImpl final : public raftpp::capnp::RaftTransport::Server {
  public:
    explicit RaftTransportImpl(CapnpTransport& owner) : owner_(owner) {}

    kj::Promise<void> sendMessages(SendMessagesContext context) override {
        auto messages = context.getParams().getMessages();
        for (auto message_reader : messages) {
            owner_.EnqueueMessage(capnp_util::clone<msg::Message>(message_reader));
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> sendSnapshot(SendSnapshotContext context) override {
        auto snapshot = context.getParams().getSnapshot();
        RAFTPP_LOG_WARN("Received snapshot via RPC (index={})", snapshot.getMetadata().getIndex());
        return kj::READY_NOW;
    }

  private:
    CapnpTransport& owner_;
};

struct RpcClient {
    RpcClient(
        std::unique_ptr<::capnp::EzRpcClient> client_in,
        raftpp::capnp::RaftTransport::Client cap_in, std::string addr_in
    )
        : client(std::move(client_in)), cap(std::move(cap_in)), addr(std::move(addr_in)) {}

    std::unique_ptr<::capnp::EzRpcClient> client;
    raftpp::capnp::RaftTransport::Client cap;
    std::string addr;
};

struct PeerRetryState {
    size_t attempts = 0;
    std::chrono::steady_clock::time_point next_attempt{};
};

struct RpcLoopFaultInjector {
    std::atomic<int> throw_after_start{0};
};

RpcLoopFaultInjector& FaultInjector() {
    static RpcLoopFaultInjector injector;
    return injector;
}

std::chrono::milliseconds PositiveReconnectInterval(std::chrono::milliseconds interval) {
    if (interval <= std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds(1);
    }
    return interval;
}

std::chrono::milliseconds ReconnectDelay(std::chrono::milliseconds base_interval, size_t attempts) {
    constexpr int kMaxReconnectMultiplier = 16;
    const int shift = static_cast<int>(
        std::min(attempts, static_cast<size_t>(std::numeric_limits<int>::digits - 1))
    );
    const int multiplier = std::min(1 << shift, kMaxReconnectMultiplier);
    return PositiveReconnectInterval(base_interval) * multiplier;
}

void MarkStopped(std::atomic<bool>& running, std::atomic<bool>& stopped) {
    running.store(false, std::memory_order_release);
    stopped.store(true, std::memory_order_release);
}

}  // namespace

CapnpTransport::CapnpTransport(TransportConfig config) : config_(std::move(config)) {}

CapnpTransport::~CapnpTransport() {
    CapnpTransport::Stop();
}

Result<void> CapnpTransport::Start() {
    telemetry::ScopedSpan span("raftor.transport.start", config_.node_id);

    if (running_) {
        return {};
    }

    // Validate address format early.
    if (auto addr_result = ParseAddress(config_.listen_addr); !addr_result) {
        telemetry::RecordErrorIf(span.span(), addr_result);
        return nonstd::make_unexpected(addr_result.error());
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
        telemetry::RecordErrorIf(span.span(), result);
        return result;
    }

    RAFTPP_LOG_INFO("CapnpTransport started on {}", config_.listen_addr);
    return result;
}

void CapnpTransport::Stop() {
    telemetry::ScopedSpan span("raftor.transport.stop", config_.node_id);

    if (!running_ || stopped_) {
        if (rpc_thread_.joinable() && rpc_thread_.get_id() != std::this_thread::get_id()) {
            rpc_thread_.join();
        }
        return;
    }

    stopped_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    if (rpc_thread_.joinable() && rpc_thread_.get_id() != std::this_thread::get_id()) {
        rpc_thread_.join();
    }

    RAFTPP_LOG_INFO("CapnpTransport stopped");
}

void CapnpTransport::AddPeer(uint64_t id, const std::string& addr) {
    std::lock_guard lock(peers_mutex_);
    peers_[id] = addr;
}

void CapnpTransport::RemovePeer(uint64_t id) {
    std::lock_guard lock(peers_mutex_);
    peers_.erase(id);
}

void CapnpTransport::Send(nonstd::span<const Message> messages) {
    telemetry::ScopedSpan span("raftor.transport.send", config_.node_id);
    span.span()->SetAttribute("raft.message.count", static_cast<int64_t>(messages.size()));

    Map<uint64_t, std::vector<Message>> batches;

    for (const auto& message : messages) {
        const auto reader = capnp_util::reader<msg::Message>(message);
        auto it = batches.find(reader.getTo());
        if (it == batches.end()) {
            it = batches.emplace(reader.getTo(), std::vector<Message>()).first;
        }
        it->second.push_back(CloneMessage(message));
    }

    if (batches.empty()) {
        return;
    }
    span.span()->SetAttribute("raft.transport.batch_count", static_cast<int64_t>(batches.size()));

    std::vector<uint64_t> dropped_peers;
    {
        std::lock_guard lock(outgoing_mutex_);
        for (auto& [peer_id, batch] : batches) {
            if (batch.empty()) {
                continue;
            }
            if (outgoing_queue_.size() >= kMaxPendingOutgoingBatches) {
                dropped_peers.push_back(peer_id);
                continue;
            }
            outgoing_queue_.push(OutgoingBatch{peer_id, std::move(batch)});
        }
    }

    for (uint64_t peer_id : dropped_peers) {
        EnqueueError(
            peer_id,
            fmt::format(
                "outgoing_queue_ overflow (capacity={}), dropping batch", kMaxPendingOutgoingBatches
            )
        );
    }
}

void CapnpTransport::SetMessageCallback(MessageCallback cb) {
    std::lock_guard lock(callback_mutex_);
    on_message_ = std::move(cb);
}

void CapnpTransport::SetErrorCallback(ErrorCallback cb) {
    std::lock_guard lock(callback_mutex_);
    on_error_ = std::move(cb);
}

void CapnpTransport::Poll(std::chrono::milliseconds timeout) {
    std::queue<Message> incoming;
    {
        std::lock_guard lock(incoming_mutex_);
        std::swap(incoming, incoming_queue_);
    }
    MessageCallback message_cb;
    ErrorCallback error_cb;
    {
        std::lock_guard lock(callback_mutex_);
        message_cb = on_message_;
        error_cb = on_error_;
    }
    while (!incoming.empty()) {
        auto msg = std::move(incoming.front());
        incoming.pop();
        if (message_cb) {
            message_cb(std::move(msg));
        }
    }

    std::queue<ErrorEvent> errors;
    {
        std::lock_guard lock(error_mutex_);
        std::swap(errors, error_queue_);
    }
    while (!errors.empty()) {
        auto error = std::move(errors.front());
        errors.pop();
        if (error_cb) {
            error_cb(error.peer_id, std::move(error.error));
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
    const auto reconnect_interval = PositiveReconnectInterval(config_.reconnect_interval);
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

        class SendTaskErrorHandler final : public kj::TaskSet::ErrorHandler {
          public:
            void taskFailed(kj::Exception&& exception) override {
                RAFTPP_LOG_WARN("RPC send task failed: {}", exception.getDescription().cStr());
            }
        } task_error_handler;

        kj::TaskSet send_tasks(task_error_handler);

        size_t inflight_send_tasks = 0;
        auto last_backpressure_log = std::chrono::steady_clock::time_point::min();

        std::unordered_map<uint64_t, std::unique_ptr<RpcClient>> clients;
        std::unordered_map<uint64_t, PeerRetryState> retry_states;

        set_start({});

        while (running_.load(std::memory_order_acquire) && !stopped_.load(std::memory_order_acquire)
        ) {
            std::vector<uint64_t> stale_clients;
            {
                std::lock_guard lock(peers_mutex_);
                for (auto it = clients.begin(); it != clients.end(); ++it) {
                    if (peers_.find(it->first) == peers_.end()) {
                        stale_clients.push_back(it->first);
                    }
                }
            }
            for (uint64_t peer_id : stale_clients) {
                clients.erase(peer_id);
                retry_states.erase(peer_id);
            }
            std::queue<OutgoingBatch> outgoing;
            uint64_t backpressure_peer = 0;
            {
                std::lock_guard lock(outgoing_mutex_);
                if (inflight_send_tasks < kMaxInFlightSendTasks) {
                    auto budget = kMaxInFlightSendTasks - inflight_send_tasks;
                    while (budget > 0 && !outgoing_queue_.empty()) {
                        outgoing.push(std::move(outgoing_queue_.front()));
                        outgoing_queue_.pop();
                        --budget;
                    }
                }
                if (outgoing.empty() && !outgoing_queue_.empty() &&
                    inflight_send_tasks >= kMaxInFlightSendTasks) {
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_backpressure_log > std::chrono::seconds(1)) {
                        backpressure_peer = outgoing_queue_.front().peer_id;
                        last_backpressure_log = now;
                    }
                }
            }

            if (backpressure_peer != 0) {
                RAFTPP_LOG_WARN(
                    "RPC send backpressure for peer {} (cap={})", backpressure_peer,
                    kMaxInFlightSendTasks
                );
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
                if (client_it != clients.end() && client_it->second->addr != addr) {
                    clients.erase(client_it);
                    retry_states.erase(batch.peer_id);
                    client_it = clients.end();
                }
                if (client_it == clients.end()) {
                    auto now = std::chrono::steady_clock::now();
                    auto& retry_state = retry_states[batch.peer_id];
                    if (retry_state.next_attempt > now) {
                        EnqueueError(
                            batch.peer_id,
                            fmt::format(
                                "RPC reconnect to {} delayed for {}ms", batch.peer_id,
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    retry_state.next_attempt - now
                                )
                                    .count()
                            )
                        );
                        continue;
                    }
                    try {
                        auto client = std::make_unique<::capnp::EzRpcClient>(addr, 0);
                        auto cap = client->getMain<raftpp::capnp::RaftTransport>();
                        client_it = clients
                                        .emplace(
                                            batch.peer_id,
                                            std::make_unique<RpcClient>(
                                                std::move(client), std::move(cap), addr
                                            )
                                        )
                                        .first;
                        retry_states.erase(batch.peer_id);
                    } catch (const kj::Exception& e) {
                        ++retry_state.attempts;
                        retry_state.next_attempt =
                            now + ReconnectDelay(reconnect_interval, retry_state.attempts - 1);
                        RAFTPP_LOG_ERROR(
                            "Failed to create RPC client for peer {} at {}: {}", batch.peer_id,
                            addr, e.getDescription().cStr()
                        );
                        EnqueueError(batch.peer_id, e.getDescription().cStr());
                        continue;
                    }
                }

                auto& cap = client_it->second->cap;
                auto req = cap.sendMessagesRequest();
                auto list = req.initMessages(batch.messages.size());
                for (size_t i = 0; i < batch.messages.size(); ++i) {
                    list.setWithCaveats(i, capnp_util::reader<msg::Message>(batch.messages[i]));
                }
                ++inflight_send_tasks;
                auto send_promise =
                    req.send()
                        .then([&inflight_send_tasks](auto&&) {
                            if (inflight_send_tasks > 0) {
                                --inflight_send_tasks;
                            }
                        })
                        .catch_([&clients, this, peer_id = batch.peer_id, &inflight_send_tasks,
                                 &retry_states, reconnect_interval](kj::Exception&& e) {
                            if (inflight_send_tasks > 0) {
                                --inflight_send_tasks;
                            }
                            RAFTPP_LOG_WARN(
                                "RPC send to {} failed: {}", peer_id, e.getDescription().cStr()
                            );
                            clients.erase(peer_id);
                            auto& retry_state = retry_states[peer_id];
                            retry_state.next_attempt =
                                std::chrono::steady_clock::now() + reconnect_interval;
                            EnqueueError(peer_id, e.getDescription().cStr());
                        });
                send_tasks.add(kj::mv(send_promise));
            }

            if (FaultInjector().throw_after_start.exchange(0, std::memory_order_acq_rel) != 0) {
                KJ_FAIL_REQUIRE("injected post-start RPC loop failure");
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
        send_tasks.clear();
        wait_scope.poll();
        wait_scope.cancelAllDetached();
        for (int i = 0; i < 1024; ++i) {
            if (wait_scope.poll() == 0) {
                break;
            }
        }
    } catch (const kj::Exception& e) {
        set_start(nonstd::make_unexpected(RaftError(RpcErrorCode::BindFailed)));
        RAFTPP_LOG_ERROR("Cap'n Proto RPC loop failed: {}", e.getDescription().cStr());
        EnqueueError(0, e.getDescription().cStr());
        MarkStopped(running_, stopped_);
    } catch (const std::exception& e) {
        set_start(nonstd::make_unexpected(RaftError(RpcErrorCode::BindFailed)));
        RAFTPP_LOG_ERROR("Cap'n Proto RPC loop failed: {}", e.what());
        EnqueueError(0, e.what());
        MarkStopped(running_, stopped_);
    } catch (...) {
        set_start(nonstd::make_unexpected(RaftError(RpcErrorCode::BindFailed)));
        RAFTPP_LOG_ERROR("Cap'n Proto RPC loop failed: unknown error");
        EnqueueError(0, "unknown error");
        MarkStopped(running_, stopped_);
    }
}

void CapnpTransportInjectPostStartLoopFailureForTest() {
    FaultInjector().throw_after_start.store(1, std::memory_order_release);
}

void CapnpTransport::EnqueueMessage(Message message) {
    uint64_t peer_id = 0;
    {
        std::lock_guard lock(incoming_mutex_);
        if (incoming_queue_.size() >= kMaxPendingIncomingMessages) {
            peer_id = capnp_util::reader<msg::Message>(message).getFrom();
        } else {
            incoming_queue_.push(std::move(message));
            return;
        }
    }
    EnqueueError(
        peer_id,
        fmt::format(
            "incoming_queue_ overflow (capacity={}), dropping message", kMaxPendingIncomingMessages
        )
    );
}

void CapnpTransport::EnqueueError(uint64_t peer_id, std::string error) {
    std::lock_guard lock(error_mutex_);
    if (error_queue_.size() >= kMaxPendingErrorEvents) {
        RAFTPP_LOG_WARN(
            "error_queue_ overflow (capacity={}) for peer {}, dropping error: {}",
            kMaxPendingErrorEvents, peer_id, error
        );
        return;
    }
    error_queue_.push(ErrorEvent{peer_id, std::move(error)});
}

}  // namespace raftpp::raftor::rpc
