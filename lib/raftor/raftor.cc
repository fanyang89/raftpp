#include "raftpp/raftor/raftor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <kj/array.h>
#include <spdlog/spdlog.h>

#include "raftpp/raftor/rpc/capnp_transport.h"
#include "raftpp/raftor/wal/wal_storage.h"
#include "ready_processor.h"

namespace raftpp::raftor {

namespace {
constexpr auto kLogSizeCheckMinInterval = std::chrono::seconds{1};
constexpr auto kSnapshotRetryMinInterval = std::chrono::seconds{1};
}  // namespace

// === RaftorConfig implementation ===

Result<void> RaftorConfig::Validate() const {
    if (node_id == 0) {
        return std::unexpected(RaftError(ConfigErrorCode::InvalidNodeId));
    }
    if (listen_addr.empty()) {
        return std::unexpected(RaftError(ConfigErrorCode::ListenAddressEmpty));
    }
    if (data_dir.empty()) {
        return std::unexpected(RaftError(ConfigErrorCode::DataDirectoryEmpty));
    }
    if (election_tick <= heartbeat_tick) {
        return std::unexpected(RaftError(ConfigErrorCode::ElectionTickTooSmall));
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
    void RefreshStatus();
    void InitializeSnapshotState();
    [[nodiscard]] uint64_t GetWalDirSizeBytes() const;

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
    if (started_.exchange(true)) {
        return std::unexpected(RaftError(RaftErrorCode::AlreadyStarted));
    }

    // Start transport
    if (auto result = transport_->Start(); !result) {
        started_ = false;
        return result;
    }

    running_ = true;
    last_tick_ = std::chrono::steady_clock::now();
    InitializeSnapshotState();

    spdlog::info("Raftor node {} started, listening on {}", config_.node_id, config_.listen_addr);
    return {};
}

void RaftorImpl::Run() {
    if (!started_) {
        spdlog::error("Cannot run: Raftor not started");
        return;
    }

    EventLoop();
}

void RaftorImpl::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // Fail all pending proposals
    proposal_tracker_.FailAll(RaftError(RaftErrorCode::ShuttingDown));
    proposal_tracker_.FailAllReads(RaftError(RaftErrorCode::ShuttingDown));

    // Stop transport
    transport_->Stop();

    started_ = false;
    spdlog::info("Raftor node {} stopped", config_.node_id);
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

        // 3. Process pending proposals from queue
        ProcessProposalQueue();

        // 4. Process pending read index requests
        ProcessReadIndexQueue();

        // 5. Process Ready if available
        if (auto result = ready_processor_->Process(); !result) {
            spdlog::error("Ready processing failed: {}", result.error().ToString());
        }

        ProcessTimeouts();
        MaybeAutoSnapshot();

        RefreshStatus();
    }
}

void RaftorImpl::Poll(std::chrono::milliseconds timeout) {
    transport_->Poll(timeout);

    if (ShouldTick()) {
        std::ignore = raw_node_->Tick();
        last_tick_ = std::chrono::steady_clock::now();
    }

    ProcessProposalQueue();
    ProcessReadIndexQueue();

    if (auto result = ready_processor_->Process(); !result) {
        spdlog::error("Ready processing failed: {}", result.error().ToString());
    }

    ProcessTimeouts();
    MaybeAutoSnapshot();

    RefreshStatus();
}

bool RaftorImpl::Tick() {
    bool ticked = raw_node_->Tick();
    last_tick_ = std::chrono::steady_clock::now();

    ProcessProposalQueue();
    ProcessReadIndexQueue();

    if (auto result = ready_processor_->Process(); !result) {
        spdlog::error("Ready processing failed: {}", result.error().ToString());
    }

    ProcessTimeouts();
    MaybeAutoSnapshot();

    RefreshStatus();
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

    spdlog::warn(
        "Snapshot init failed to read first index: {}", first_index_result.error().ToString()
    );
    last_snapshot_attempt_index_ = 0;
}

uint64_t RaftorImpl::GetWalDirSizeBytes() const {
    return storage_ ? storage_->LogSizeBytes() : 0;
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
        spdlog::error(
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

    if (applied_index <= last_snapshot_attempt_index_ &&
        now - last_snapshot_attempt_time_ < kSnapshotRetryMinInterval) {
        return;
    }

    last_snapshot_attempt_index_ = applied_index;
    last_snapshot_attempt_time_ = now;
    spdlog::info(
        "Auto snapshot triggered (reason={}, applied_index={}, snapshot_index={})",
        reason ? reason : "unknown", applied_index, snapshot_index
    );

    if (auto result = TakeSnapshot(); !result) {
        spdlog::error("Auto snapshot failed: {}", result.error().ToString());
    }
}

void RaftorImpl::ProcessProposalQueue() {
    while (auto item = proposal_queue_.TryPop()) {
        auto& [data, callback] = *item;

        // Generate a unique context for tracking this proposal
        std::string ctx = GenerateProposalContext();

        // Track the proposal
        proposal_tracker_.Track(ctx, std::move(callback), config_.proposal_timeout);

        // Submit to Raft
        if (auto result = raw_node_->Propose(ctx, data); !result) {
            proposal_tracker_.Fail(ctx, result.error());
        }
    }
}

void RaftorImpl::ProcessReadIndexQueue() {
    while (auto item = read_index_queue_.TryPop()) {
        auto& [ctx, callback] = *item;

        // Track the read
        proposal_tracker_.TrackRead(ctx, std::move(callback), config_.read_index_timeout);

        // Submit to Raft
        raw_node_->ReadIndex(ctx);
    }
}

std::string RaftorImpl::GenerateProposalContext() {
    uint64_t counter = proposal_counter_.fetch_add(1);
    return std::to_string(config_.node_id) + ":" + std::to_string(counter);
}

void RaftorImpl::ProcessTimeouts() {
    proposal_tracker_.ExpireTimeouts(std::chrono::steady_clock::now());
}

void RaftorImpl::OnMessage(Message msg) {
    if (auto result = raw_node_->Step(std::move(msg)); !result) {
        // Step errors are usually benign (e.g., stale messages)
        spdlog::debug("Step error: {}", result.error().ToString());
    }
}

void RaftorImpl::OnPeerError(uint64_t peer_id, std::string error) {
    spdlog::warn("Peer {} error: {}", peer_id, error);
    raw_node_->ReportUnreachable(peer_id);
    state_machine_->OnPeerUnreachable(peer_id);
}

void RaftorImpl::Propose(std::string data, ProposalCallback callback) {
    proposal_queue_.Push(std::move(data), std::move(callback));
}

Result<std::string> RaftorImpl::ProposeSync(std::string data, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<Result<std::string>>>();
    auto future = promise->get_future();
    auto completed = std::make_shared<std::atomic<bool>>(false);

    Propose(std::move(data), [promise, completed](Result<std::string> result) {
        if (completed->exchange(true)) {
            return;
        }
        promise->set_value(std::move(result));
    });

    if (future.wait_for(timeout) == std::future_status::timeout) {
        if (completed->exchange(true)) {
            return future.get();
        }
        return std::unexpected(RaftError(RpcErrorCode::Timeout));
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
    read_index_queue_.Push(std::move(ctx), std::move(callback));
}

Result<void> RaftorImpl::ReadIndexSync(std::string ctx, std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<Result<void>>>();
    auto future = promise->get_future();
    auto completed = std::make_shared<std::atomic<bool>>(false);

    ReadIndex(std::move(ctx), [promise, completed](Result<void> result) {
        if (completed->exchange(true)) {
            return;
        }
        promise->set_value(std::move(result));
    });

    if (future.wait_for(timeout) == std::future_status::timeout) {
        if (completed->exchange(true)) {
            return future.get();
        }
        return std::unexpected(RaftError(RpcErrorCode::Timeout));
    }

    return future.get();
}

Result<void> RaftorImpl::AddNode(uint64_t id, const std::string& addr) {
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
        return result.error();
    }

    // Add to transport immediately (will be used once connected)
    transport_->AddPeer(id, addr);

    return {};
}

Result<void> RaftorImpl::RemoveNode(uint64_t id) {
    ConfChangeV2 cc = capnp_util::make<msg::ConfChangeV2>();
    auto builder = capnp_util::builder<msg::ConfChangeV2>(cc);
    auto changes = builder.initChanges(1);
    changes[0].setChangeType(ConfChangeType::REMOVE_NODE);
    changes[0].setNodeId(id);

    std::string ctx = GenerateProposalContext();
    return raw_node_->ProposeConfChange(ctx, cc);
}

void RaftorImpl::TransferLeader(uint64_t target_id) {
    raw_node_->TransferLeader(target_id);
}

Result<void> RaftorImpl::Campaign() {
    return raw_node_->Campaign();
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
    auto status = raw_node_->GetStatus();
    auto applied_index = ready_processor_->GetAppliedIndex();

    // Get current conf state from storage
    auto initial_state = storage_->InitialState();
    if (!initial_state) {
        return initial_state.error();
    }

    // Get term of applied entry
    auto term_result = storage_->Term(applied_index);
    if (!term_result) {
        return term_result.error();
    }

    // Create snapshot via state machine
    auto snapshot_result =
        state_machine_->TakeSnapshot(applied_index, *term_result, initial_state->conf_state);
    if (!snapshot_result) {
        return snapshot_result.error();
    }

    // Create Cap'n Proto snapshot
    Snapshot snapshot = capnp_util::make<msg::Snapshot>();
    auto snap_builder = capnp_util::builder<msg::Snapshot>(snapshot);
    snap_builder.setData(
        kj::arrayPtr(
            reinterpret_cast<const kj::byte*>(snapshot_result->data.data()),
            snapshot_result->data.size()
        )
    );
    snap_builder.setMetadata(capnp_util::reader<msg::SnapshotMetadata>(snapshot_result->metadata));

    // Apply to storage (this will compact the log)
    if (auto result = storage_->ApplySnapshot(snapshot); !result) {
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

    // Create TCP transport
    rpc::TransportConfig transport_config;
    transport_config.listen_addr = config.listen_addr;
    transport_config.node_id = config.node_id;
    transport_config.connect_timeout = config.connect_timeout;

    auto transport = std::make_unique<rpc::CapnpTransport>(transport_config);

    return Create(
        config, std::move(state_machine), std::move(*storage_result), std::move(transport)
    );
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
        return std::unexpected(RaftError(RaftErrorCode::IncompatibleStorage));
    }

    return std::make_unique<RaftorImpl>(
        config, std::move(state_machine), std::move(wal_storage), std::move(transport)
    );
}

}  // namespace raftpp::raftor
