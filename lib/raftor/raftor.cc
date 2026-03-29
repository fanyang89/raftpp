#include "raftpp/raftor/raftor.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#include <kj/array.h>

#include "raftpp/logging.h"
#include "raftpp/raftor/rpc/capnp_transport.h"
#include "raftpp/raftor/rpc/codec.h"
#include "raftpp/raftor/telemetry.h"
#if defined(RAFTPP_WITH_RDMA) && RAFTPP_WITH_RDMA
#include "raftpp/raftor/rpc/rdma_transport.h"
#endif
#include "raftpp/raftor/wal/wal_storage.h"
#include "ready_processor.h"

namespace raftpp::raftor {

#ifndef RAFTPP_WITH_RDMA
#define RAFTPP_WITH_RDMA 0
#endif

namespace {
constexpr auto kLogSizeCheckMinInterval = std::chrono::seconds{1};
constexpr auto kSnapshotRetryMinInterval = std::chrono::seconds{1};

class TempFileSnapshotWriter final : public SnapshotWriter {
  public:
    explicit TempFileSnapshotWriter(std::FILE* file) : file_(file) {}

    Result<void> Write(nonstd::span<const uint8_t> chunk) override {
        const size_t bytes_written = std::fwrite(chunk.data(), 1, chunk.size(), file_);
        if (bytes_written != chunk.size()) {
            return RaftError(
                StorageErrorOther{
                    fmt::format("snapshot temp write failed: {}", std::strerror(errno)),
                }
            );
        }
        total_bytes_written_ += bytes_written;
        return {};
    }

    [[nodiscard]] uint64_t total_bytes_written() const { return total_bytes_written_; }

  private:
    std::FILE* file_;
    uint64_t total_bytes_written_ = 0;
};

Result<void> LoadSnapshotDataFromFile(
    std::FILE* file, uint64_t payload_size, msg::Snapshot::Builder* snapshot_builder
) {
    if (payload_size > std::numeric_limits<uint32_t>::max()) {
        return RaftError(
            StorageErrorOther{
                "snapshot payload exceeds Cap'n Proto Data size limit",
            }
        );
    }

    if (std::fseek(file, 0, SEEK_SET) != 0) {
        return RaftError(
            StorageErrorOther{
                fmt::format("snapshot temp rewind failed: {}", std::strerror(errno)),
            }
        );
    }

    auto data_builder = snapshot_builder->initData(static_cast<uint32_t>(payload_size));
    size_t copied = 0;
    while (copied < payload_size) {
        const size_t to_read =
            std::min(static_cast<size_t>(payload_size - copied), size_t{1 << 20});
        size_t read = std::fread(data_builder.begin() + copied, 1, to_read, file);
        if (read == 0) {
            if (std::ferror(file) != 0) {
                return RaftError(
                    StorageErrorOther{
                        fmt::format("snapshot temp read failed: {}", std::strerror(errno)),
                    }
                );
            }
            return RaftError(
                StorageErrorOther{
                    "snapshot temp read hit unexpected EOF",
                }
            );
        }
        copied += read;
    }

    return {};
}

bool TryGetRdmaMaxFrameSize(uint64_t payload_max, size_t* max_frame_size) {
    const size_t frame_overhead = rpc::Codec::FrameOverhead();
    const size_t message_overhead = rpc::Codec::MessageOverhead();
    if (message_overhead > std::numeric_limits<size_t>::max() - frame_overhead) {
        return false;
    }
    const size_t total_overhead = frame_overhead + message_overhead;
    if (payload_max > std::numeric_limits<size_t>::max() - total_overhead) {
        return false;
    }
    *max_frame_size = static_cast<size_t>(payload_max) + total_overhead;
    return true;
}
}  // namespace

// === RaftorConfig implementation ===

Result<void> RaftorConfig::Validate() const {
    if (node_id == 0) {
        return nonstd::make_unexpected(RaftError(ConfigErrorCode::InvalidNodeId));
    }
    if (listen_addr.empty()) {
        return nonstd::make_unexpected(RaftError(ConfigErrorCode::ListenAddressEmpty));
    }
    if (data_dir.empty()) {
        return nonstd::make_unexpected(RaftError(ConfigErrorCode::DataDirectoryEmpty));
    }
    if (election_tick <= heartbeat_tick) {
        return nonstd::make_unexpected(RaftError(ConfigErrorCode::ElectionTickTooSmall));
    }
    if (transport_kind == TransportKind::Rdma) {
        if (rdma.recv_buffer_count == 0 || rdma.send_buffer_count == 0 || rdma.buffer_size == 0 ||
            rdma.cq_depth == 0 || rdma.qp_depth == 0) {
            return nonstd::make_unexpected(RaftError(ConfigErrorCode::RdmaConfigInvalid));
        }
        if (rdma.recv_buffer_count > rdma.qp_depth || rdma.send_buffer_count > rdma.qp_depth) {
            return nonstd::make_unexpected(RaftError(ConfigErrorCode::RdmaConfigInvalid));
        }
        if (rdma.max_inline_data > rdma.buffer_size) {
            return nonstd::make_unexpected(RaftError(ConfigErrorCode::RdmaConfigInvalid));
        }
        constexpr auto kMaxU32 = std::numeric_limits<uint32_t>::max();
        if (rdma.buffer_size > kMaxU32 || rdma.recv_buffer_count > kMaxU32 ||
            rdma.send_buffer_count > kMaxU32 || rdma.cq_depth > kMaxU32 ||
            rdma.qp_depth > kMaxU32 || rdma.max_inline_data > kMaxU32) {
            return nonstd::make_unexpected(RaftError(ConfigErrorCode::RdmaConfigInvalid));
        }
        size_t max_frame_size = 0;
        if (!TryGetRdmaMaxFrameSize(max_size_per_message, &max_frame_size)) {
            return nonstd::make_unexpected(RaftError(ConfigErrorCode::RdmaConfigInvalid));
        }
        if (rdma.buffer_size < max_frame_size) {
            return nonstd::make_unexpected(RaftError(ConfigErrorCode::RdmaConfigInvalid));
        }
        const auto cq_needed =
            static_cast<uint64_t>(rdma.recv_buffer_count) + rdma.send_buffer_count;
        if (cq_needed > rdma.cq_depth) {
            return nonstd::make_unexpected(RaftError(ConfigErrorCode::RdmaConfigInvalid));
        }
    }
    return {};
}

raftpp::Config RaftorConfig::ToRaftConfig() const {
    raftpp::Config cfg;
    cfg.id = node_id;
    cfg.election_tick = election_tick;
    cfg.heartbeat_tick = heartbeat_tick;
    cfg.max_size_per_message = max_size_per_message;
    cfg.max_inflight_messages = max_inflight_messages;
    cfg.pre_vote = pre_vote;
    cfg.check_quorum = check_quorum;
    cfg.read_only_option = read_only_option;
    return cfg;
}

// === RaftorImpl implementation ===

class RaftorImpl : public Raftor {
  public:
    RaftorImpl(
        const RaftorConfig& config, std::unique_ptr<StateMachine> state_machine,
        std::shared_ptr<wal::WALStorage> storage, std::unique_ptr<rpc::Transport> transport
    );

    ~RaftorImpl() override;

    // Lifecycle
    Result<void> Start() override;
    void Run() override;
    void Stop() override;
    bool IsRunning() const override;

    // Proposals
    void Propose(std::string data, ProposalCallback callback) override;
    Result<std::string> ProposeSync(std::string data, std::chrono::milliseconds timeout) override;
    std::future<Result<std::string>> ProposeAsync(std::string data) override;

    // Reads
    void ReadIndex(std::string ctx, ReadIndexCallback callback) override;
    Result<void> ReadIndexSync(std::string ctx, std::chrono::milliseconds timeout) override;

    // Cluster management
    Result<void> AddNode(uint64_t id, const std::string& addr) override;
    Result<void> RemoveNode(uint64_t id) override;
    void TransferLeader(uint64_t target_id) override;
    Result<void> Campaign() override;

    // Status
    NodeStatus GetStatus() const override;
    bool IsLeader() const override;
    uint64_t GetLeaderId() const override;

    // Advanced
    Result<void> TakeSnapshot() override;
    RawNode& GetRawNode() override;
    void Poll(std::chrono::milliseconds timeout) override;
    bool Tick() override;

  private:
    void EventLoop();
    void ProcessProposalQueue();
    void ProcessReadIndexQueue();
    void ProcessTimeouts();
    bool ShouldTick();
    void MaybeAutoSnapshot();
    void OnMessage(Message msg);
    void OnPeerError(uint64_t peer_id, std::string error);
    std::string GenerateProposalContext();
    void EnqueueProposal(
        std::string data, ProposalCallback callback, std::chrono::milliseconds timeout
    );
    void EnqueueReadIndex(
        std::string ctx, ReadIndexCallback callback, std::chrono::milliseconds timeout
    );
    void RefreshStatus();
    void InitializeSnapshotState();
    [[nodiscard]] uint64_t GetWalDirSizeBytes() const;
    void ProcessRaftWork();

    RaftorConfig config_;
    std::unique_ptr<StateMachine> state_machine_;
    std::shared_ptr<wal::WALStorage> storage_;
    std::unique_ptr<rpc::Transport> transport_;
    std::unique_ptr<RawNode> raw_node_;
    std::unique_ptr<ReadyProcessor> ready_processor_;

    ProposalTracker proposal_tracker_;
    ProposalQueue proposal_queue_;
    ReadIndexQueue read_index_queue_;

    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};

    // Tick timing
    std::chrono::steady_clock::time_point last_tick_;

    // Proposal context counter
    std::atomic<uint64_t> proposal_counter_{0};

    // Cache status for thread-safe access from non-event-loop threads.
    mutable std::mutex status_mutex_;
    NodeStatus cached_status_{};

    // Auto snapshot tracking
    uint64_t last_snapshot_attempt_index_ = 0;
    std::chrono::steady_clock::time_point last_snapshot_attempt_time_{};
    std::chrono::steady_clock::time_point last_snapshot_time_{};
    std::chrono::steady_clock::time_point last_log_size_check_{};
};

RaftorImpl::RaftorImpl(
    const RaftorConfig& config, std::unique_ptr<StateMachine> state_machine,
    std::shared_ptr<wal::WALStorage> storage, std::unique_ptr<rpc::Transport> transport
)
    : config_(config),
      state_machine_(std::move(state_machine)),
      storage_(std::move(storage)),
      transport_(std::move(transport)) {
    // Create RawNode
    auto raft_config = config_.ToRaftConfig();
    raw_node_ = std::make_unique<RawNode>(raft_config, storage_);

    // Create ReadyProcessor
    ready_processor_ = std::make_unique<ReadyProcessor>(
        *raw_node_, storage_, *state_machine_, *transport_, proposal_tracker_
    );

    RefreshStatus();

    // Set up transport callbacks
    transport_->SetMessageCallback([this](Message msg) { OnMessage(std::move(msg)); });
    transport_->SetErrorCallback([this](uint64_t peer_id, std::string error) {
        OnPeerError(peer_id, std::move(error));
    });

    // Add initial peers to transport
    for (const auto& peer : config_.initial_peers) {
        if (peer.id != config_.node_id) {
            transport_->AddPeer(peer.id, peer.addr);
        }
    }
}

RaftorImpl::~RaftorImpl() {
    Stop();
}

Result<void> RaftorImpl::Start() {
    telemetry::ScopedSpan span("raftor.start", config_.node_id);

    if (started_.exchange(true)) {
        telemetry::RecordError(span.span(), "already started");
        RAFTPP_LOG_WARN("Start ignored: Raftor {} already started", config_.node_id);
        return nonstd::make_unexpected(RaftError(RaftErrorCode::AlreadyStarted));
    }

    // Start transport
    if (auto result = transport_->Start(); !result) {
        started_ = false;
        telemetry::RecordErrorIf(span.span(), result);
        RAFTPP_LOG_ERROR(
            "Failed to start transport for node {}: {}", config_.node_id, result.error().ToString()
        );
        return result;
    }

    running_ = true;
    last_tick_ = std::chrono::steady_clock::now();
    InitializeSnapshotState();

    RAFTPP_LOG_INFO(
        "Raftor node {} started, listening on {}", config_.node_id, config_.listen_addr
    );
    return {};
}

void RaftorImpl::Run() {
    if (!started_) {
        RAFTPP_LOG_ERROR("Cannot run: Raftor not started");
        return;
    }

    EventLoop();
}

void RaftorImpl::Stop() {
    telemetry::ScopedSpan span("raftor.stop", config_.node_id);

    if (!running_.exchange(false)) {
        return;
    }

    const auto shutdown_error = RaftError(RaftErrorCode::ShuttingDown);

    // Fail requests still waiting in the cross-thread queues.
    while (auto item = proposal_queue_.TryPop()) {
        if (item->callback) {
            item->callback(nonstd::make_unexpected(shutdown_error));
        }
    }
    while (auto item = read_index_queue_.TryPop()) {
        if (item->callback) {
            item->callback(nonstd::make_unexpected(shutdown_error));
        }
    }

    // Fail all pending proposals
    proposal_tracker_.FailAll(shutdown_error);
    proposal_tracker_.FailAllReads(shutdown_error);

    // Stop transport
    transport_->Stop();

    started_ = false;
    RAFTPP_LOG_INFO("Raftor node {} stopped", config_.node_id);
}

bool RaftorImpl::IsRunning() const {
    return running_;
}

void RaftorImpl::EventLoop() {
    while (running_) {
        // 1. Poll network with tick interval timeout
        transport_->Poll(config_.tick_interval);

        // 2. Check tick timer
        if (ShouldTick()) {
            std::ignore = raw_node_->Tick();
            last_tick_ = std::chrono::steady_clock::now();
        }

        ProcessRaftWork();
    }
}

void RaftorImpl::Poll(std::chrono::milliseconds timeout) {
    transport_->Poll(timeout);

    if (ShouldTick()) {
        std::ignore = raw_node_->Tick();
        last_tick_ = std::chrono::steady_clock::now();
    }

    ProcessRaftWork();
}

bool RaftorImpl::Tick() {
    bool ticked = raw_node_->Tick();
    last_tick_ = std::chrono::steady_clock::now();

    ProcessRaftWork();
    return ticked;
}

bool RaftorImpl::ShouldTick() {
    auto now = std::chrono::steady_clock::now();
    return (now - last_tick_) >= config_.tick_interval;
}

void RaftorImpl::InitializeSnapshotState() {
    last_snapshot_time_ = std::chrono::steady_clock::now();
    last_snapshot_attempt_time_ = std::chrono::steady_clock::time_point{};
    last_log_size_check_ = std::chrono::steady_clock::time_point{};

    auto first_index_result = storage_->FirstIndex();
    if (first_index_result) {
        const uint64_t first_index = *first_index_result;
        last_snapshot_attempt_index_ = first_index > 0 ? first_index - 1 : 0;
        return;
    }

    RAFTPP_LOG_WARN(
        "Snapshot init failed to read first index: {}", first_index_result.error().ToString()
    );
    last_snapshot_attempt_index_ = 0;
}

uint64_t RaftorImpl::GetWalDirSizeBytes() const {
    return storage_ ? storage_->LogSizeBytes() : 0;
}

void RaftorImpl::ProcessRaftWork() {
    telemetry::ScopedSpan span("raftor.process_work", config_.node_id);
    span.span()->SetAttribute(
        "raft.pending_proposals", static_cast<int64_t>(proposal_tracker_.PendingCount())
    );
    span.span()->SetAttribute(
        "raft.pending_reads", static_cast<int64_t>(proposal_tracker_.PendingReadCount())
    );
    span.span()->SetAttribute("raft.queue.proposals", static_cast<int64_t>(proposal_queue_.Size()));

    ProcessProposalQueue();

    if (auto result = ready_processor_->Process(); !result) {
        RAFTPP_LOG_ERROR("Ready processing failed: {}", result.error().ToString());
        telemetry::RecordErrorIf(span.span(), result);
    }

    ProcessReadIndexQueue();

    ProcessTimeouts();
    MaybeAutoSnapshot();
    RefreshStatus();
}

void RaftorImpl::MaybeAutoSnapshot() {
    if (config_.snapshot_entries_threshold == 0 && config_.snapshot_log_size_bytes == 0 &&
        config_.snapshot_interval.count() == 0) {
        return;
    }

    const uint64_t applied_index = ready_processor_->GetAppliedIndex();
    if (applied_index == 0) {
        return;
    }

    auto first_index_result = storage_->FirstIndex();
    if (!first_index_result) {
        RAFTPP_LOG_ERROR(
            "Auto snapshot skipped: failed to read first index: {}",
            first_index_result.error().ToString()
        );
        return;
    }

    const uint64_t first_index = *first_index_result;
    const uint64_t snapshot_index = first_index > 0 ? first_index - 1 : 0;
    if (applied_index <= snapshot_index) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    bool should_snapshot = false;
    const char* reason = nullptr;

    if (config_.snapshot_entries_threshold > 0 &&
        applied_index - snapshot_index >= config_.snapshot_entries_threshold) {
        should_snapshot = true;
        reason = "entries";
    } else if (config_.snapshot_interval.count() > 0 &&
               now - last_snapshot_time_ >= config_.snapshot_interval) {
        should_snapshot = true;
        reason = "time";
    } else if (config_.snapshot_log_size_bytes > 0) {
        if (now - last_log_size_check_ >= kLogSizeCheckMinInterval) {
            last_log_size_check_ = now;
            const uint64_t wal_size = GetWalDirSizeBytes();
            if (wal_size >= config_.snapshot_log_size_bytes) {
                should_snapshot = true;
                reason = "log_size";
            }
        }
    }

    if (!should_snapshot) {
        return;
    }

    telemetry::ScopedSpan span("raftor.snapshot.auto", config_.node_id);
    span.span()->SetAttribute("raft.snapshot.applied_index", static_cast<int64_t>(applied_index));
    span.span()->SetAttribute("raft.snapshot.index", static_cast<int64_t>(snapshot_index));
    if (reason) {
        span.span()->SetAttribute("raft.snapshot.reason", reason);
    }

    if (applied_index <= last_snapshot_attempt_index_ &&
        now - last_snapshot_attempt_time_ < kSnapshotRetryMinInterval) {
        span.span()->SetAttribute("raft.snapshot.skipped", true);
        return;
    }

    last_snapshot_attempt_index_ = applied_index;
    last_snapshot_attempt_time_ = now;
    RAFTPP_LOG_INFO(
        "Auto snapshot triggered (reason={}, applied_index={}, snapshot_index={})",
        reason ? reason : "unknown", applied_index, snapshot_index
    );

    if (auto result = TakeSnapshot(); !result) {
        RAFTPP_LOG_ERROR("Auto snapshot failed: {}", result.error().ToString());
        telemetry::RecordErrorIf(span.span(), result);
    }
}

void RaftorImpl::ProcessProposalQueue() {
    while (auto item = proposal_queue_.TryPop()) {
        auto data = std::move(item->data);
        auto callback = std::move(item->callback);
        const auto timeout = item->timeout.value_or(config_.proposal_timeout);

        // Generate a unique context for tracking this proposal
        std::string ctx = GenerateProposalContext();

        telemetry::ScopedSpan span("raftor.proposal.process", config_.node_id);
        span.span()->SetAttribute("raft.proposal.ctx", ctx);
        span.span()->SetAttribute("raft.proposal.data_bytes", static_cast<int64_t>(data.size()));
        span.span()->SetAttribute(
            "raft.proposal.timeout_ms", static_cast<int64_t>(timeout.count())
        );

        // Track the proposal
        proposal_tracker_.Track(ctx, std::move(callback), timeout);

        // Submit to Raft
        if (auto result = raw_node_->Propose(ctx, data); !result) {
            proposal_tracker_.Fail(ctx, result.error());
            telemetry::RecordErrorIf(span.span(), result);
            RAFTPP_LOG_ERROR("Proposal {} failed: {}", ctx, result.error().ToString());
        }
    }
}

void RaftorImpl::ProcessReadIndexQueue() {
    while (auto item = read_index_queue_.TryPop()) {
        auto ctx = std::move(item->ctx);
        auto callback = std::move(item->callback);
        const auto timeout = item->timeout.value_or(config_.read_index_timeout);

        telemetry::ScopedSpan span("raftor.read_index.process", config_.node_id);
        span.span()->SetAttribute("raft.read.ctx_bytes", static_cast<int64_t>(ctx.size()));
        span.span()->SetAttribute("raft.read.timeout_ms", static_cast<int64_t>(timeout.count()));

        // Track the read
        proposal_tracker_.TrackRead(ctx, std::move(callback), timeout);

        // Submit to Raft
        raw_node_->ReadIndex(ctx);
    }
}

std::string RaftorImpl::GenerateProposalContext() {
    uint64_t counter = proposal_counter_.fetch_add(1);
    return std::to_string(config_.node_id) + ":" + std::to_string(counter);
}

void RaftorImpl::EnqueueProposal(
    std::string data, ProposalCallback callback, std::chrono::milliseconds timeout
) {
    auto span = telemetry::StartSpanWithNodeId("raftor.proposal", config_.node_id);
    span->SetAttribute("raft.proposal.data_bytes", static_cast<int64_t>(data.size()));
    span->SetAttribute("raft.proposal.timeout_ms", static_cast<int64_t>(timeout.count()));

    auto wrapped_callback = [span,
                             callback = std::move(callback)](Result<std::string> result) mutable {
        telemetry::RecordErrorIf(span, result);
        span->End();
        if (callback) {
            callback(std::move(result));
        }
    };

    proposal_queue_.Push(std::move(data), std::move(wrapped_callback), timeout);
}

void RaftorImpl::EnqueueReadIndex(
    std::string ctx, ReadIndexCallback callback, std::chrono::milliseconds timeout
) {
    auto span = telemetry::StartSpanWithNodeId("raftor.read_index", config_.node_id);
    span->SetAttribute("raft.read.ctx_bytes", static_cast<int64_t>(ctx.size()));
    span->SetAttribute("raft.read.timeout_ms", static_cast<int64_t>(timeout.count()));

    auto wrapped_callback = [span, callback = std::move(callback)](Result<void> result) mutable {
        telemetry::RecordErrorIf(span, result);
        span->End();
        if (callback) {
            callback(std::move(result));
        }
    };

    read_index_queue_.Push(std::move(ctx), std::move(wrapped_callback), timeout);
}

void RaftorImpl::ProcessTimeouts() {
    proposal_tracker_.ExpireTimeouts(std::chrono::steady_clock::now());
}

void RaftorImpl::OnMessage(Message msg) {
    telemetry::ScopedSpan span("raftor.step", config_.node_id);
    const auto reader = capnp_util::reader<msg::Message>(msg);
    span.span()->SetAttribute("raft.msg.type", static_cast<int64_t>(reader.getMsgType()));
    span.span()->SetAttribute("raft.msg.from", static_cast<int64_t>(reader.getFrom()));
    span.span()->SetAttribute("raft.msg.to", static_cast<int64_t>(reader.getTo()));
    span.span()->SetAttribute("raft.msg.term", static_cast<int64_t>(reader.getTerm()));
    span.span()->SetAttribute("raft.msg.index", static_cast<int64_t>(reader.getIndex()));
    span.span()->SetAttribute("raft.msg.commit", static_cast<int64_t>(reader.getCommit()));

    if (auto result = raw_node_->Step(std::move(msg)); !result) {
        // Step errors are usually benign (e.g., stale messages)
        RAFTPP_LOG_DEBUG("Step error: {}", result.error().ToString());
        telemetry::RecordErrorIf(span.span(), result);
    }
}

void RaftorImpl::OnPeerError(uint64_t peer_id, std::string error) {
    telemetry::ScopedSpan span("raftor.peer_error", config_.node_id);
    span.span()->SetAttribute("raft.peer_id", static_cast<int64_t>(peer_id));
    telemetry::RecordError(span.span(), error);

    RAFTPP_LOG_WARN("Peer {} error: {}", peer_id, error);
    raw_node_->ReportUnreachable(peer_id);
    state_machine_->OnPeerUnreachable(peer_id);
}

void RaftorImpl::Propose(std::string data, ProposalCallback callback) {
    EnqueueProposal(std::move(data), std::move(callback), config_.proposal_timeout);
}

Result<std::string> RaftorImpl::ProposeSync(std::string data, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<Result<std::string>>>();
    auto future = promise->get_future();
    auto completed = std::make_shared<std::atomic<bool>>(false);

    EnqueueProposal(
        std::move(data),
        [promise, completed](Result<std::string> result) {
            if (completed->exchange(true)) {
                return;
            }
            promise->set_value(std::move(result));
        },
        timeout
    );

    if (future.wait_for(timeout) == std::future_status::timeout) {
        if (completed->exchange(true)) {
            return future.get();
        }
        return nonstd::make_unexpected(RaftError(RpcErrorCode::Timeout));
    }

    return future.get();
}

std::future<Result<std::string>> RaftorImpl::ProposeAsync(std::string data) {
    auto promise = std::make_shared<std::promise<Result<std::string>>>();
    auto future = promise->get_future();

    Propose(std::move(data), [promise](Result<std::string> result) {
        promise->set_value(std::move(result));
    });

    return future;
}

void RaftorImpl::ReadIndex(std::string ctx, ReadIndexCallback callback) {
    EnqueueReadIndex(std::move(ctx), std::move(callback), config_.read_index_timeout);
}

Result<void> RaftorImpl::ReadIndexSync(std::string ctx, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<Result<void>>>();
    auto future = promise->get_future();
    auto completed = std::make_shared<std::atomic<bool>>(false);

    EnqueueReadIndex(
        std::move(ctx),
        [promise, completed](Result<void> result) {
            if (completed->exchange(true)) {
                return;
            }
            promise->set_value(std::move(result));
        },
        timeout
    );

    if (future.wait_for(timeout) == std::future_status::timeout) {
        if (completed->exchange(true)) {
            return future.get();
        }
        return nonstd::make_unexpected(RaftError(RpcErrorCode::Timeout));
    }

    return future.get();
}

Result<void> RaftorImpl::AddNode(uint64_t id, const std::string& addr) {
    telemetry::ScopedSpan span("raftor.conf_change.add_node", config_.node_id);
    span.span()->SetAttribute("raft.peer_id", static_cast<int64_t>(id));
    span.span()->SetAttribute("raft.peer.addr_bytes", static_cast<int64_t>(addr.size()));

    ConfChangeV2 cc = capnp_util::make<msg::ConfChangeV2>();
    auto builder = capnp_util::builder<msg::ConfChangeV2>(cc);
    auto changes = builder.initChanges(1);
    changes[0].setChangeType(ConfChangeType::ADD_NODE);
    changes[0].setNodeId(id);
    builder.setContext(
        kj::arrayPtr(reinterpret_cast<const kj::byte*>(addr.data()), addr.size())
    );  // Store address in context

    std::string ctx = GenerateProposalContext();
    if (auto result = raw_node_->ProposeConfChange(ctx, cc); !result) {
        telemetry::RecordErrorIf(span.span(), result);
        RAFTPP_LOG_ERROR("Add node {} failed: {}", id, result.error().ToString());
        return result.error();
    }

    // Add to transport immediately (will be used once connected)
    transport_->AddPeer(id, addr);

    return {};
}

Result<void> RaftorImpl::RemoveNode(uint64_t id) {
    telemetry::ScopedSpan span("raftor.conf_change.remove_node", config_.node_id);
    span.span()->SetAttribute("raft.peer_id", static_cast<int64_t>(id));

    ConfChangeV2 cc = capnp_util::make<msg::ConfChangeV2>();
    auto builder = capnp_util::builder<msg::ConfChangeV2>(cc);
    auto changes = builder.initChanges(1);
    changes[0].setChangeType(ConfChangeType::REMOVE_NODE);
    changes[0].setNodeId(id);

    std::string ctx = GenerateProposalContext();
    auto result = raw_node_->ProposeConfChange(ctx, cc);
    telemetry::RecordErrorIf(span.span(), result);
    if (!result) {
        RAFTPP_LOG_ERROR("Remove node {} failed: {}", id, result.error().ToString());
    }
    return result;
}

void RaftorImpl::TransferLeader(uint64_t target_id) {
    telemetry::ScopedSpan span("raftor.transfer_leader", config_.node_id);
    span.span()->SetAttribute("raft.target_id", static_cast<int64_t>(target_id));

    raw_node_->TransferLeader(target_id);
}

Result<void> RaftorImpl::Campaign() {
    telemetry::ScopedSpan span("raftor.campaign", config_.node_id);

    auto result = raw_node_->Campaign();
    telemetry::RecordErrorIf(span.span(), result);
    if (!result) {
        RAFTPP_LOG_ERROR("Campaign failed: {}", result.error().ToString());
    }
    return result;
}

NodeStatus RaftorImpl::GetStatus() const {
    std::lock_guard lock(status_mutex_);
    return cached_status_;
}

bool RaftorImpl::IsLeader() const {
    std::lock_guard lock(status_mutex_);
    return cached_status_.role == StateRole::Leader;
}

uint64_t RaftorImpl::GetLeaderId() const {
    std::lock_guard lock(status_mutex_);
    return cached_status_.leader_id;
}

Result<void> RaftorImpl::TakeSnapshot() {
    telemetry::ScopedSpan span("raftor.snapshot.create", config_.node_id);

    auto status = raw_node_->GetStatus();
    auto applied_index = ready_processor_->GetAppliedIndex();
    span.span()->SetAttribute("raft.snapshot.applied_index", static_cast<int64_t>(applied_index));
    span.span()->SetAttribute("raft.role", static_cast<int64_t>(status.ss.raft_state));
    auto hs_reader = capnp_util::reader<msg::HardState>(status.hs);
    span.span()->SetAttribute("raft.term", static_cast<int64_t>(hs_reader.getTerm()));
    span.span()->SetAttribute("raft.commit", static_cast<int64_t>(hs_reader.getCommit()));

    // Get current conf state from storage
    auto initial_state = storage_->InitialState();
    if (telemetry::RecordErrorIf(span.span(), initial_state)) {
        RAFTPP_LOG_ERROR(
            "Snapshot failed to read initial state: {}", initial_state.error().ToString()
        );
        return initial_state.error();
    }

    // Get term of applied entry
    auto term_result = storage_->Term(applied_index);
    if (telemetry::RecordErrorIf(span.span(), term_result)) {
        RAFTPP_LOG_ERROR(
            "Snapshot failed to read term at index {}: {}", applied_index,
            term_result.error().ToString()
        );
        return term_result.error();
    }

    std::unique_ptr<std::FILE, int (*)(std::FILE*)> snapshot_file(std::tmpfile(), &std::fclose);
    if (snapshot_file == nullptr) {
        auto error = RaftError(
            StorageErrorOther{
                fmt::format("snapshot temp file creation failed: {}", std::strerror(errno)),
            }
        );
        telemetry::RecordError(span.span(), error.ToString());
        RAFTPP_LOG_ERROR("Snapshot failed to create temp file: {}", error.ToString());
        return error;
    }

    // Create snapshot via state machine and stream payload to a temp file.
    TempFileSnapshotWriter writer(snapshot_file.get());
    auto metadata_result = state_machine_->TakeSnapshot(
        applied_index, *term_result, initial_state->conf_state, writer
    );
    if (telemetry::RecordErrorIf(span.span(), metadata_result)) {
        RAFTPP_LOG_ERROR(
            "Snapshot failed to build state at index {}: {}", applied_index,
            metadata_result.error().ToString()
        );
        return metadata_result.error();
    }

    // Create Cap'n Proto snapshot
    Snapshot snapshot = capnp_util::make<msg::Snapshot>();
    auto snap_builder = capnp_util::builder<msg::Snapshot>(snapshot);
    const uint64_t payload_size = writer.total_bytes_written();
    span.span()->SetAttribute("raft.snapshot.payload_bytes", static_cast<int64_t>(payload_size));

    auto load_result = LoadSnapshotDataFromFile(snapshot_file.get(), payload_size, &snap_builder);
    if (telemetry::RecordErrorIf(span.span(), load_result)) {
        RAFTPP_LOG_ERROR(
            "Snapshot failed to load payload from temp file at index {}: {}", applied_index,
            load_result.error().ToString()
        );
        return load_result.error();
    }
    snap_builder.setMetadata(capnp_util::reader<msg::SnapshotMetadata>(*metadata_result));

    // Apply to storage (this will compact the log)
    if (auto result = storage_->ApplySnapshot(snapshot); !result) {
        telemetry::RecordErrorIf(span.span(), result);
        RAFTPP_LOG_ERROR(
            "Snapshot apply failed at index {}: {}", applied_index, result.error().ToString()
        );
        return result;
    }

    last_snapshot_time_ = std::chrono::steady_clock::now();
    last_snapshot_attempt_index_ = applied_index;
    return {};
}

RawNode& RaftorImpl::GetRawNode() {
    return *raw_node_;
}

void RaftorImpl::RefreshStatus() {
    auto status = raw_node_->GetStatus();
    NodeStatus ns;
    ns.id = status.id;
    ns.role = status.ss.raft_state;
    ns.term = capnp_util::reader<msg::HardState>(status.hs).getTerm();
    ns.leader_id = status.ss.leader_id;
    ns.commit_index = capnp_util::reader<msg::HardState>(status.hs).getCommit();
    ns.applied_index = status.applied;
    ns.pending_proposals = proposal_tracker_.PendingCount();

    std::lock_guard lock(status_mutex_);
    cached_status_ = ns;
}

// === Factory methods ===

Result<std::unique_ptr<Raftor>> Raftor::Create(
    const RaftorConfig& config, std::unique_ptr<StateMachine> state_machine
) {
    // Validate config
    if (auto result = config.Validate(); !result) {
        return result.error();
    }

    // Create WAL storage
    wal::WALConfig wal_config;
    wal_config.dir = config.data_dir / "wal";
    wal_config.sync_on_write = true;

    auto storage_result = wal::WALStorage::Open(wal_config);
    if (!storage_result) {
        return storage_result.error();
    }

    auto storage = std::move(*storage_result);

    // Bootstrap if WAL is uninitialized
    if (!storage->IsInitialized()) {
        ConfState conf_state = capnp_util::make<msg::ConfState>();
        auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);

        if (config.initial_peers.empty()) {
            // Single-node cluster: bootstrap with only this node
            RAFTPP_LOG_INFO("Bootstrapping single-node cluster with node {}", config.node_id);
            auto voters = conf_builder.initVoters(1);
            voters.set(0, config.node_id);
        } else {
            // Multi-node cluster: validate and use initial_peers
            bool node_id_found = false;
            for (const auto& peer : config.initial_peers) {
                if (peer.id == config.node_id) {
                    node_id_found = true;
                    break;
                }
            }

            if (!node_id_found) {
                return nonstd::make_unexpected(RaftError(ConfigErrorCode::NodeIdNotInInitialPeers));
            }

            RAFTPP_LOG_INFO(
                "Bootstrapping cluster with {} initial peers", config.initial_peers.size()
            );
            auto voters = conf_builder.initVoters(config.initial_peers.size());
            for (size_t i = 0; i < config.initial_peers.size(); ++i) {
                voters.set(i, config.initial_peers[i].id);
            }
        }

        // Persist bootstrap ConfState to WAL
        storage->SetConfState(conf_state);
        RAFTPP_LOG_INFO(
            "WAL bootstrap complete: node {} initialized with cluster configuration", config.node_id
        );
    } else {
        // WAL already initialized - ignore initial_peers and use existing config
        RAFTPP_LOG_INFO("WAL already initialized, ignoring initial_peers");
    }

    // Create RPC transport
    rpc::TransportConfig transport_config;
    transport_config.listen_addr = config.listen_addr;
    transport_config.node_id = config.node_id;
    transport_config.connect_timeout = config.connect_timeout;
    transport_config.max_message_size = config.max_size_per_message;
    if (config.transport_kind == TransportKind::Rdma) {
        size_t max_frame_size = 0;
        if (TryGetRdmaMaxFrameSize(config.max_size_per_message, &max_frame_size)) {
            transport_config.max_message_size = max_frame_size;
        }
    }

    std::unique_ptr<rpc::Transport> transport;
    switch (config.transport_kind) {
        case TransportKind::Capnp:
            transport = std::make_unique<rpc::CapnpTransport>(transport_config);
            break;
        case TransportKind::Rdma:
#if RAFTPP_WITH_RDMA
            transport = std::make_unique<rpc::RdmaTransport>(transport_config, config.rdma);
#else
            RAFTPP_LOG_WARN("RDMA transport requested but not enabled at build time");
            return nonstd::make_unexpected(RaftError(ConfigErrorCode::RdmaNotEnabled));
#endif
            break;
    }

    return Create(config, std::move(state_machine), std::move(storage), std::move(transport));
}

Result<std::unique_ptr<Raftor>> Raftor::Create(
    const RaftorConfig& config, std::unique_ptr<StateMachine> state_machine,
    std::shared_ptr<Storage> storage, std::unique_ptr<rpc::Transport> transport
) {
    // Validate config
    if (auto result = config.Validate(); !result) {
        return result.error();
    }

    // Cast to WALStorage if possible
    auto wal_storage = std::dynamic_pointer_cast<wal::WALStorage>(storage);
    if (!wal_storage) {
        return nonstd::make_unexpected(RaftError(RaftErrorCode::IncompatibleStorage));
    }

    return std::make_unique<RaftorImpl>(
        config, std::move(state_machine), std::move(wal_storage), std::move(transport)
    );
}

}  // namespace raftpp::raftor
