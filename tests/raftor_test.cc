#include "raftpp/raftor/raftor.h"

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>
#include <nonstd/expected.hpp>
#include <nonstd/span.hpp>

#include "../lib/raftor/metadata_change.h"
#include "raftpp/core/capnp_util.h"
#include "raftpp/core/memory_storage.h"
#include "raftpp/core/types.h"
#include "raftpp/logging.h"
#include "raftpp/raftor/raftor_config.h"
#include "raftpp/raftor/rpc/noop_transport.h"
#include "raftpp/raftor/rpc/transport.h"
#include "raftpp/raftor/state_machine.h"
#include "raftpp/raftor/wal/wal_config.h"
#include "raftpp/raftor/wal/wal_storage.h"

using namespace raftpp;
using namespace raftpp::raftor;
using namespace std::chrono_literals;

namespace {

class TempDirCleanup {
  public:
    explicit TempDirCleanup(std::filesystem::path path) : path_(std::move(path)) {}

    TempDirCleanup(const TempDirCleanup&) = delete;
    TempDirCleanup& operator=(const TempDirCleanup&) = delete;

    ~TempDirCleanup() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        if (ec) {
            RAFTPP_LOG_WARN("Failed to remove temp directory {}: {}", path_.string(), ec.message());
        }
    }

  private:
    std::filesystem::path path_;
};

class PortAllocator {
  public:
    static uint16_t GetNextPort() {
        static std::atomic<uint16_t> next_port{19000};
        return next_port.fetch_add(1);
    }
};

class MockStateMachine : public StateMachine {
  public:
    Result<ApplyResult> Apply(const Entry& entry) override {
        std::lock_guard lock(mutex_);
        auto reader = capnp_util::reader<msg::Entry>(entry);
        auto data = reader.getData();
        applied_entries_.emplace_back(data.begin(), data.end());
        apply_count_++;
        return ApplyResult{"OK:" + std::to_string(apply_count_)};
    }

    Result<SnapshotMetadata> TakeSnapshot(
        uint64_t applied_index, uint64_t applied_term, const ConfState& conf_state,
        SnapshotWriter& writer
    ) override {
        std::lock_guard lock(mutex_);
        const std::array<uint8_t, 4> snapshot_payload = {'s', 'n', 'a', 'p'};
        if (auto write_result = writer.Write(snapshot_payload); !write_result) {
            return nonstd::make_unexpected(write_result.error());
        }

        auto metadata = capnp_util::make<msg::SnapshotMetadata>();
        auto meta_builder = capnp_util::builder<msg::SnapshotMetadata>(metadata);
        meta_builder.setIndex(applied_index);
        meta_builder.setTerm(applied_term);
        meta_builder.setConfState(capnp_util::reader<msg::ConfState>(conf_state));
        return metadata;
    }

    Result<void> RestoreSnapshot(const SnapshotMetadata& metadata, SnapshotReader& reader)
        override {
        std::lock_guard lock(mutex_);
        if (fail_restore_) {
            return nonstd::make_unexpected(RaftError(StorageErrorCode::Unavailable));
        }

        auto meta_reader = capnp_util::reader<msg::SnapshotMetadata>(metadata);
        last_restored_index_ = meta_reader.getIndex();
        last_restored_term_ = meta_reader.getTerm();
        last_restored_data_.clear();

        std::array<uint8_t, 1024> buffer{};
        while (true) {
            auto read_result = reader.Read(buffer);
            if (!read_result) {
                return nonstd::make_unexpected(read_result.error());
            }
            if (*read_result == 0) {
                break;
            }
            last_restored_data_.insert(
                last_restored_data_.end(), buffer.begin(), buffer.begin() + *read_result
            );
        }
        restore_count_++;
        return {};
    }

    void OnLeadershipChange(bool is_leader, uint64_t term, uint64_t leader_id) override {
        std::lock_guard lock(mutex_);
        leadership_changes_.push_back({is_leader, term, leader_id});
        RAFTPP_LOG_INFO(
            "Leadership change: is_leader={}, term={}, leader_id={}", is_leader, term, leader_id
        );
    }

    size_t ApplyCount() const {
        std::lock_guard lock(mutex_);
        return apply_count_;
    }

    std::vector<std::vector<uint8_t>> GetAppliedEntries() const {
        std::lock_guard lock(mutex_);
        return applied_entries_;
    }

    size_t RestoreCount() const {
        std::lock_guard lock(mutex_);
        return restore_count_;
    }

    uint64_t LastRestoredIndex() const {
        std::lock_guard lock(mutex_);
        return last_restored_index_;
    }

    std::vector<uint8_t> LastRestoredData() const {
        std::lock_guard lock(mutex_);
        return last_restored_data_;
    }

    std::vector<std::tuple<bool, uint64_t, uint64_t>> GetLeadershipChanges() const {
        std::lock_guard lock(mutex_);
        return leadership_changes_;
    }

    void SetFailRestore(bool fail) {
        std::lock_guard lock(mutex_);
        fail_restore_ = fail;
    }

  private:
    mutable std::mutex mutex_;
    size_t apply_count_ = 0;
    size_t restore_count_ = 0;
    bool fail_restore_ = false;
    uint64_t last_restored_index_ = 0;
    uint64_t last_restored_term_ = 0;
    std::vector<std::vector<uint8_t>> applied_entries_;
    std::vector<uint8_t> last_restored_data_;
    std::vector<std::tuple<bool, uint64_t, uint64_t>> leadership_changes_;
};

class RecordingTransport final : public rpc::Transport {
  public:
    Result<void> Start() override {
        running_ = true;
        return {};
    }

    void Stop() override { running_ = false; }

    void AddPeer(uint64_t id, const std::string& addr) override {
        std::lock_guard lock(mutex_);
        peers_.push_back({id, addr});
    }

    void RemovePeer(uint64_t id) override {
        std::lock_guard lock(mutex_);
        removed_peers_.push_back(id);
    }

    void Send(nonstd::span<const Message> /*messages*/) override {}

    void SetMessageCallback(rpc::MessageCallback cb) override { message_callback_ = std::move(cb); }

    void SetErrorCallback(rpc::ErrorCallback cb) override { error_callback_ = std::move(cb); }

    void Poll(std::chrono::milliseconds /*timeout*/) override {}

    void Run() override { running_ = true; }

    std::vector<PeerConfig> Peers() const {
        std::lock_guard lock(mutex_);
        return peers_;
    }

    std::vector<uint64_t> RemovedPeers() const {
        std::lock_guard lock(mutex_);
        return removed_peers_;
    }

  private:
    mutable std::mutex mutex_;
    bool running_ = false;
    std::vector<PeerConfig> peers_;
    std::vector<uint64_t> removed_peers_;
    rpc::MessageCallback message_callback_;
    rpc::ErrorCallback error_callback_;
};

class FaultyStorage final : public WritableStorage {
  public:
    [[nodiscard]] Result<RaftState> InitialState() override {
        if (fail_initial_state) {
            return RaftError(StorageErrorCode::Unavailable);
        }
        return storage_->InitialState();
    }

    [[nodiscard]] Result<std::vector<Entry>> Entries(
        uint64_t low, uint64_t high, std::optional<uint64_t> max_size, GetEntriesContext context
    ) override {
        return storage_->Entries(low, high, max_size, context);
    }

    [[nodiscard]] Result<uint64_t> Term(uint64_t idx) override { return storage_->Term(idx); }

    [[nodiscard]] Result<uint64_t> FirstIndex() override { return storage_->FirstIndex(); }

    [[nodiscard]] Result<uint64_t> LastIndex() override { return storage_->LastIndex(); }

    [[nodiscard]] Result<Snapshot> GetSnapshot(uint64_t request_index, uint64_t to) override {
        return storage_->GetSnapshot(request_index, to);
    }

    [[nodiscard]] Result<void> Append(const std::vector<Entry>& entries) override {
        return storage_->Append(entries);
    }

    [[nodiscard]] Result<void> SetHardState(HardState&& hs) override {
        if (fail_set_hard_state) {
            return RaftError(StorageErrorCode::Unavailable);
        }
        return storage_->SetHardState(std::move(hs));
    }

    [[nodiscard]] Result<void> SetConfState(const ConfState& conf_state) override {
        if (fail_set_conf_state) {
            return RaftError(StorageErrorCode::Unavailable);
        }
        return storage_->SetConfState(conf_state);
    }

    [[nodiscard]] Result<void> ApplySnapshot(const Snapshot& snapshot) override {
        return storage_->ApplySnapshot(snapshot);
    }

    [[nodiscard]] Result<void> Sync() override {
        if (fail_sync) {
            return RaftError(StorageErrorCode::Unavailable);
        }
        return storage_->Sync();
    }

    [[nodiscard]] Result<std::optional<Snapshot>> LocalSnapshot() override {
        if (fail_local_snapshot) {
            return RaftError(StorageErrorCode::Unavailable);
        }
        if (!local_snapshot) {
            return std::nullopt;
        }
        return std::optional<Snapshot>{CloneSnapshot(*local_snapshot)};
    }

    [[nodiscard]] uint64_t LogSizeBytes() const override { return log_size_bytes; }

    bool fail_initial_state = false;
    bool fail_set_hard_state = false;
    bool fail_set_conf_state = false;
    bool fail_sync = false;
    bool fail_local_snapshot = false;
    uint64_t log_size_bytes = 0;
    std::optional<Snapshot> local_snapshot;

  private:
    std::shared_ptr<MemoryStorage> storage_ = std::make_shared<MemoryStorage>();
};

struct TestNode {
    std::unique_ptr<Raftor> raftor;
    RaftorConfig config;
    MockStateMachine* state_machine = nullptr;
    std::filesystem::path temp_dir;
    std::unique_ptr<TempDirCleanup> temp_dir_cleanup;
};

Result<TestNode> CreateTestNode(
    uint64_t node_id, const std::string& listen_addr, const std::vector<PeerConfig>& initial_peers
) {
    TestNode node;

    const auto pid = static_cast<uint64_t>(::getpid());
    node.temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_test_" + std::to_string(pid) + "_" + std::to_string(node_id));
    std::error_code ec;
    std::filesystem::remove_all(node.temp_dir, ec);
    std::filesystem::create_directories(node.temp_dir);
    node.temp_dir_cleanup = std::make_unique<TempDirCleanup>(node.temp_dir);

    node.config.node_id = node_id;
    node.config.listen_addr = listen_addr;
    node.config.data_dir = node.temp_dir;
    node.config.election_tick = 10;
    node.config.heartbeat_tick = 1;
    node.config.tick_interval = 10ms;
    node.config.pre_vote = true;
    node.config.check_quorum = true;
    node.config.initial_peers = initial_peers;

    auto state_machine = std::make_unique<MockStateMachine>();
    node.state_machine = state_machine.get();

    auto raftor_result = Raftor::Create(node.config, std::move(state_machine));
    if (!raftor_result) {
        return nonstd::make_unexpected(raftor_result.error());
    }
    node.raftor = std::move(*raftor_result);

    return node;
}

void PollAll(std::vector<std::unique_ptr<Raftor>>& raftors, std::chrono::milliseconds duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& r : raftors) {
            r->Poll(1ms);
        }
        std::this_thread::sleep_for(1ms);
    }
}

bool HasLeader(const std::vector<std::unique_ptr<Raftor>>& raftors) {
    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        if (status.role == StateRole::Leader) {
            return true;
        }
    }
    return false;
}

bool HasStableLeader(const std::vector<std::unique_ptr<Raftor>>& raftors) {
    uint64_t leader_id = 0;
    int leader_count = 0;

    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        if (status.role == StateRole::Leader) {
            leader_count++;
            leader_id = status.id;
        }
    }

    if (leader_count != 1 || leader_id == 0) {
        return false;
    }

    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        if (status.role == StateRole::Candidate || status.role == StateRole::PreCandidate) {
            return false;
        }
        if (status.leader_id != leader_id) {
            return false;
        }
    }
    return true;
}

bool WaitForStableLeader(
    std::vector<std::unique_ptr<Raftor>>& raftors, std::chrono::milliseconds timeout,
    std::chrono::milliseconds step = 25ms
) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        PollAll(raftors, step);
        if (HasStableLeader(raftors)) {
            return true;
        }
    }
    return HasStableLeader(raftors);
}

uint64_t GetLeaderId(const std::vector<std::unique_ptr<Raftor>>& raftors) {
    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        if (status.role == StateRole::Leader) {
            return status.id;
        }
    }
    return 0;
}

}  // namespace

TEST_SUITE_BEGIN("raftor_bootstrap");

TEST_CASE("three_node_cluster_bootstrap") {
    RAFTPP_LOG_INFO("Creating 3-node cluster...");

    std::vector<PeerConfig> peers = {
        PeerConfig{1, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        PeerConfig{2, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        PeerConfig{3, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
    };

    RAFTPP_LOG_INFO("Peers configured:");
    for (const auto& peer : peers) {
        RAFTPP_LOG_INFO("  ID: {}, addr: {}", peer.id, peer.addr);
    }

    std::vector<TestNode> test_nodes;

    for (auto peer : peers) {
        auto result = CreateTestNode(peer.id, peer.addr, peers);
        if (!result) {
            RAFTPP_LOG_ERROR("Error creating node {}: {}", peer.id, result.error().ToString());
        }
        REQUIRE(result.has_value());
        test_nodes.push_back(std::move(*result));
    }

    std::vector<std::unique_ptr<Raftor>> raftors;
    for (auto& node : test_nodes) {
        auto start_result = node.raftor->Start();
        if (!start_result) {
            RAFTPP_LOG_ERROR("Error starting node: {}", start_result.error().ToString());
        }
        REQUIRE(start_result.has_value());
        raftors.push_back(std::move(node.raftor));
    }

    RAFTPP_LOG_INFO("Polling for leader election (up to 2s)...");
    REQUIRE_MESSAGE(WaitForStableLeader(raftors, 2s), "No leader was elected");

    for (size_t i = 0; i < raftors.size(); ++i) {
        auto status = raftors[i]->GetStatus();
        RAFTPP_LOG_INFO(
            "Node {}: role={}, term={}, leader_id={}", status.id, static_cast<int>(status.role),
            status.term, status.leader_id
        );
    }

    uint64_t leader_id = GetLeaderId(raftors);
    REQUIRE_MESSAGE(leader_id != 0, "Leader ID should not be zero");

    int leader_count = 0;
    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        if (status.role == StateRole::Leader) {
            leader_count++;
        }
    }
    REQUIRE_MESSAGE(
        leader_count == 1, "Exactly one leader should be elected, got {}", leader_count
    );

    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        REQUIRE_MESSAGE(
            status.leader_id == leader_id, "All nodes should know the leader ID {}",
            status.leader_id
        );
    }
}

TEST_CASE("conf_state_initialized_from_initial_peers") {
    std::string listen_addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort());
    std::vector<PeerConfig> peers = {
        PeerConfig{1, listen_addr},
        PeerConfig{2, ""},
        PeerConfig{3, ""},
    };

    auto result = CreateTestNode(1, listen_addr, peers);
    REQUIRE(result.has_value());
    TestNode node = std::move(*result);

    auto raftor = std::move(node.raftor);
    auto status = raftor->GetStatus();

    REQUIRE_MESSAGE(status.role != StateRole::Leader, "Node should not be leader without quorum");
}

TEST_CASE("noop_transport_allows_empty_listen_address") {
    TestNode node;
    const auto pid = static_cast<uint64_t>(::getpid());
    node.temp_dir =
        std::filesystem::temp_directory_path() / ("raftpp_noop_test_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(node.temp_dir, ec);
    std::filesystem::create_directories(node.temp_dir);
    node.temp_dir_cleanup = std::make_unique<TempDirCleanup>(node.temp_dir);

    node.config.node_id = 1;
    node.config.transport_kind = TransportKind::Noop;
    node.config.data_dir = node.temp_dir;

    auto state_machine = std::make_unique<MockStateMachine>();
    auto raftor_result = Raftor::Create(node.config, std::move(state_machine));
    REQUIRE(raftor_result.has_value());
}

TEST_CASE("wal_peer_addresses_persist_across_reopen") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir =
        std::filesystem::temp_directory_path() / ("raftpp_peer_address_wal_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    wal::WALConfig wal_config;
    wal_config.dir = temp_dir / "wal";
    wal_config.sync_on_write = true;

    auto storage_result = wal::WALStorage::Open(wal_config);
    REQUIRE(storage_result.has_value());
    auto storage = std::move(*storage_result);

    auto save_result = storage->SetPeerAddresses({
        wal::PeerAddress{1, "127.0.0.1:19101"},
        wal::PeerAddress{2, "127.0.0.1:19102"},
    });
    REQUIRE(save_result.has_value());
    storage.reset();

    auto reopened_result = wal::WALStorage::Open(wal_config);
    REQUIRE(reopened_result.has_value());
    auto peers = (*reopened_result)->GetPeerAddresses();

    REQUIRE(peers.size() == 2);
    CHECK(peers[0].id == 1);
    CHECK(peers[0].addr == "127.0.0.1:19101");
    CHECK(peers[1].id == 2);
    CHECK(peers[1].addr == "127.0.0.1:19102");
}

TEST_CASE("raftor_metadata_change_round_trip") {
    MetadataChange change;
    change.type = MetadataChangeType::UpsertPeerAddress;
    change.node_id = 42;
    change.addr = "127.0.0.1:19420";

    auto data = SerializeMetadataChange(change);
    auto parsed = ParseMetadataChange(data);

    REQUIRE(parsed.has_value());
    CHECK(parsed->type == MetadataChangeType::UpsertPeerAddress);
    CHECK(parsed->node_id == 42);
    CHECK(parsed->addr == "127.0.0.1:19420");
}

TEST_CASE("raftor_metadata_change_rejects_invalid_data") {
    CHECK(IsMetadataProposalContext(std::string(kMetadataProposalContextPrefix) + "ctx"));
    CHECK(!IsMetadataProposalContext("raftor:proposal:ctx"));

    auto parsed = ParseMetadataChange("not a capnp message");
    CHECK(!parsed.has_value());

    auto msg = capnp_util::make<msg::RaftorMetadataChange>();
    capnp_util::builder<msg::RaftorMetadataChange>(msg).setRemovePeerAddress(42);
    auto reserved = ParseMetadataChange(capnp_util::toString(msg));
    CHECK(!reserved.has_value());
}

TEST_CASE("update_node_address_validates_request") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_peer_address_validate_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    wal::WALConfig wal_config;
    wal_config.dir = temp_dir / "wal";
    auto storage_result = wal::WALStorage::Open(wal_config);
    REQUIRE(storage_result.has_value());
    auto storage = std::move(*storage_result);

    ConfState conf_state = capnp_util::make<msg::ConfState>();
    auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
    auto voters = conf_builder.initVoters(1);
    voters.set(0, 1);
    REQUIRE(storage->SetConfState(conf_state));
    REQUIRE(storage->SetPeerAddresses({wal::PeerAddress{1, "127.0.0.1:19401"}}).has_value());

    RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:19401";
    config.data_dir = temp_dir;

    auto transport = std::make_unique<RecordingTransport>();
    auto state_machine = std::make_unique<MockStateMachine>();
    auto raftor_result =
        Raftor::Create(config, std::move(state_machine), storage, std::move(transport));
    REQUIRE(raftor_result.has_value());
    auto raftor = std::move(*raftor_result);

    CHECK(!raftor->UpdateNodeAddress(0, "127.0.0.1:19402").has_value());
    CHECK(!raftor->UpdateNodeAddress(1, "").has_value());
    CHECK(!raftor->UpdateNodeAddress(2, "127.0.0.1:19402").has_value());
}

TEST_CASE("raftor_restores_transport_peers_from_wal_address_book") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_peer_address_transport_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    wal::WALConfig wal_config;
    wal_config.dir = temp_dir / "wal";
    auto storage_result = wal::WALStorage::Open(wal_config);
    REQUIRE(storage_result.has_value());
    auto storage = std::move(*storage_result);

    ConfState conf_state = capnp_util::make<msg::ConfState>();
    auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
    auto voters = conf_builder.initVoters(2);
    voters.set(0, 1);
    voters.set(1, 2);
    REQUIRE(storage->SetConfState(conf_state));
    REQUIRE(storage
                ->SetPeerAddresses({
                    wal::PeerAddress{1, "127.0.0.1:19201"},
                    wal::PeerAddress{2, "127.0.0.1:19202"},
                })
                .has_value());

    RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:19201";
    config.data_dir = temp_dir;
    config.initial_peers = {
        PeerConfig{1, "ignored-self"},
        PeerConfig{2, "ignored-peer"},
    };

    auto transport = std::make_unique<RecordingTransport>();
    auto* recording_transport = transport.get();
    auto state_machine = std::make_unique<MockStateMachine>();

    auto raftor_result =
        Raftor::Create(config, std::move(state_machine), std::move(storage), std::move(transport));
    REQUIRE(raftor_result.has_value());

    auto peers = recording_transport->Peers();
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].id == 2);
    CHECK(peers[0].addr == "127.0.0.1:19202");
}

TEST_CASE("update_node_address_commits_to_wal_metadata") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_peer_address_update_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    wal::WALConfig wal_config;
    wal_config.dir = temp_dir / "wal";
    auto storage_result = wal::WALStorage::Open(wal_config);
    REQUIRE(storage_result.has_value());
    auto storage = std::move(*storage_result);

    ConfState conf_state = capnp_util::make<msg::ConfState>();
    auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
    auto voters = conf_builder.initVoters(1);
    voters.set(0, 1);
    REQUIRE(storage->SetConfState(conf_state));
    REQUIRE(storage->SetPeerAddresses({wal::PeerAddress{1, "127.0.0.1:19301"}}).has_value());

    RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:19301";
    config.data_dir = temp_dir;
    config.election_tick = 5;
    config.heartbeat_tick = 1;
    config.tick_interval = 1ms;

    auto transport = std::make_unique<RecordingTransport>();
    auto* recording_transport = transport.get();
    auto state_machine = std::make_unique<MockStateMachine>();
    auto* mock_state_machine = state_machine.get();

    auto raftor_result =
        Raftor::Create(config, std::move(state_machine), storage, std::move(transport));
    REQUIRE(raftor_result.has_value());
    auto raftor = std::move(*raftor_result);
    REQUIRE(raftor->Start());
    REQUIRE(raftor->Campaign());

    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!raftor->IsLeader() && std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(1ms);
    }
    REQUIRE(raftor->IsLeader());

    REQUIRE(raftor->UpdateNodeAddress(1, "127.0.0.1:19302"));
    deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(1ms);
        auto peers = storage->GetPeerAddresses();
        if (!peers.empty() && peers[0].addr == "127.0.0.1:19302") {
            break;
        }
    }

    auto peers = storage->GetPeerAddresses();
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].id == 1);
    CHECK(peers[0].addr == "127.0.0.1:19302");
    CHECK(mock_state_machine->GetAppliedEntries().empty());

    auto removed_peers = recording_transport->RemovedPeers();
    REQUIRE(!removed_peers.empty());
    CHECK(removed_peers.back() == 1);
}

TEST_CASE("duplicate read index contexts both complete") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_duplicate_read_index_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    auto listen_addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort());
    RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = listen_addr;
    config.data_dir = temp_dir;
    config.election_tick = 5;
    config.heartbeat_tick = 1;
    config.tick_interval = 1ms;
    config.initial_peers = {PeerConfig{1, listen_addr}};

    auto storage = std::make_shared<MemoryStorage>();
    auto transport = std::make_unique<RecordingTransport>();
    auto state_machine = std::make_unique<MockStateMachine>();
    auto raftor_result =
        Raftor::Create(config, std::move(state_machine), storage, std::move(transport));
    REQUIRE(raftor_result.has_value());
    auto raftor = std::move(*raftor_result);

    REQUIRE(raftor->Start());
    REQUIRE(raftor->Campaign());

    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!raftor->IsLeader() && std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(1ms);
    }
    REQUIRE(raftor->IsLeader());

    std::promise<Result<std::string>> proposal_promise;
    auto proposal_future = proposal_promise.get_future();
    raftor->Propose("readindex_warmup", [&proposal_promise](Result<std::string> result) {
        proposal_promise.set_value(std::move(result));
    });

    deadline = std::chrono::steady_clock::now() + 1s;
    while (proposal_future.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(1ms);
    }
    REQUIRE(proposal_future.wait_for(0ms) == std::future_status::ready);
    REQUIRE(proposal_future.get().has_value());

    auto read_promise1 = std::make_shared<std::promise<Result<void>>>();
    auto read_promise2 = std::make_shared<std::promise<Result<void>>>();
    auto read_future1 = read_promise1->get_future();
    auto read_future2 = read_promise2->get_future();
    auto completed1 = std::make_shared<std::atomic<bool>>(false);
    auto completed2 = std::make_shared<std::atomic<bool>>(false);

    raftor->ReadIndex("same-context", [read_promise1, completed1](Result<void> result) {
        if (!completed1->exchange(true)) {
            read_promise1->set_value(std::move(result));
        }
    });
    raftor->ReadIndex("same-context", [read_promise2, completed2](Result<void> result) {
        if (!completed2->exchange(true)) {
            read_promise2->set_value(std::move(result));
        }
    });

    deadline = std::chrono::steady_clock::now() + 1s;
    while ((read_future1.wait_for(0ms) != std::future_status::ready ||
            read_future2.wait_for(0ms) != std::future_status::ready) &&
           std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(1ms);
    }

    REQUIRE(read_future1.wait_for(0ms) == std::future_status::ready);
    REQUIRE(read_future2.wait_for(0ms) == std::future_status::ready);
    CHECK(read_future1.get().has_value());
    CHECK(read_future2.get().has_value());
    CHECK(completed1->load());
    CHECK(completed2->load());
}

TEST_CASE("add_node_commits_peer_address_to_wal_metadata") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_peer_address_add_node_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    wal::WALConfig wal_config;
    wal_config.dir = temp_dir / "wal";
    auto storage_result = wal::WALStorage::Open(wal_config);
    REQUIRE(storage_result.has_value());
    auto storage = std::move(*storage_result);

    ConfState conf_state = capnp_util::make<msg::ConfState>();
    auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
    auto voters = conf_builder.initVoters(1);
    voters.set(0, 1);
    REQUIRE(storage->SetConfState(conf_state));
    REQUIRE(storage->SetPeerAddresses({wal::PeerAddress{1, "127.0.0.1:19501"}}).has_value());

    RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:19501";
    config.data_dir = temp_dir;
    config.election_tick = 5;
    config.heartbeat_tick = 1;
    config.tick_interval = 1ms;

    auto transport = std::make_unique<RecordingTransport>();
    auto* recording_transport = transport.get();
    auto state_machine = std::make_unique<MockStateMachine>();

    auto raftor_result =
        Raftor::Create(config, std::move(state_machine), storage, std::move(transport));
    REQUIRE(raftor_result.has_value());
    auto raftor = std::move(*raftor_result);
    REQUIRE(raftor->Start());
    REQUIRE(raftor->Campaign());

    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!raftor->IsLeader() && std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(1ms);
    }
    REQUIRE(raftor->IsLeader());

    REQUIRE(raftor->AddNode(2, "127.0.0.1:19502"));
    deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(1ms);
        auto transport_peers = recording_transport->Peers();
        if (!transport_peers.empty()) {
            break;
        }
    }

    auto peers = storage->GetPeerAddresses();
    REQUIRE(peers.size() == 2);
    CHECK(peers[1].id == 2);
    CHECK(peers[1].addr == "127.0.0.1:19502");

    auto transport_peers = recording_transport->Peers();
    REQUIRE(transport_peers.size() == 1);
    CHECK(transport_peers[0].id == 2);
    CHECK(transport_peers[0].addr == "127.0.0.1:19502");
}

TEST_CASE("custom_writable_storage_create_accepts_memory_storage") {
    TestNode node;
    const auto pid = static_cast<uint64_t>(::getpid());
    node.temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_custom_storage_test_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(node.temp_dir, ec);
    std::filesystem::create_directories(node.temp_dir);
    node.temp_dir_cleanup = std::make_unique<TempDirCleanup>(node.temp_dir);

    node.config.node_id = 1;
    node.config.transport_kind = TransportKind::Noop;
    node.config.data_dir = node.temp_dir;
    node.config.election_tick = 5;
    node.config.heartbeat_tick = 1;

    auto storage = std::make_shared<MemoryStorage>();
    rpc::TransportConfig transport_config;
    transport_config.node_id = node.config.node_id;
    auto transport = std::make_unique<rpc::NoopTransport>(transport_config);

    auto state_machine = std::make_unique<MockStateMachine>();
    auto raftor_result =
        Raftor::Create(node.config, std::move(state_machine), storage, std::move(transport));
    REQUIRE(raftor_result.has_value());

    auto state = storage->InitialState();
    REQUIRE(state.has_value());
    auto conf_reader = capnp_util::reader<msg::ConfState>(state->conf_state);
    REQUIRE(conf_reader.getVoters().size() == 1);
    CHECK(conf_reader.getVoters()[0] == 1);
}

TEST_CASE("custom_writable_storage_create_does_not_restore_synthetic_snapshot") {
    TestNode node;
    const auto pid = static_cast<uint64_t>(::getpid());
    node.temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_custom_storage_restart_test_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(node.temp_dir, ec);
    std::filesystem::create_directories(node.temp_dir);
    node.temp_dir_cleanup = std::make_unique<TempDirCleanup>(node.temp_dir);

    node.config.node_id = 1;
    node.config.transport_kind = TransportKind::Noop;
    node.config.data_dir = node.temp_dir;
    node.config.election_tick = 5;
    node.config.heartbeat_tick = 1;

    auto storage = std::make_shared<MemoryStorage>();

    ConfState conf_state = capnp_util::make<msg::ConfState>();
    auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
    auto voters = conf_builder.initVoters(1);
    voters.set(0, 1);
    REQUIRE(storage->SetConfState(conf_state));

    Entry entry = capnp_util::make<msg::Entry>();
    auto entry_builder = capnp_util::builder<msg::Entry>(entry);
    entry_builder.setIndex(1);
    entry_builder.setTerm(1);
    std::vector<Entry> entries;
    entries.push_back(std::move(entry));
    REQUIRE(storage->Append(entries));

    HardState hard_state = capnp_util::make<msg::HardState>();
    auto hs_builder = capnp_util::builder<msg::HardState>(hard_state);
    hs_builder.setTerm(1);
    hs_builder.setCommit(1);
    REQUIRE(storage->SetHardState(std::move(hard_state)));

    rpc::TransportConfig transport_config;
    transport_config.node_id = node.config.node_id;
    auto transport = std::make_unique<rpc::NoopTransport>(transport_config);

    auto state_machine = std::make_unique<MockStateMachine>();
    auto* state_machine_ptr = state_machine.get();
    auto raftor_result =
        Raftor::Create(node.config, std::move(state_machine), storage, std::move(transport));
    REQUIRE(raftor_result.has_value());
    CHECK(state_machine_ptr->RestoreCount() == 0);
}

TEST_CASE("custom_writable_storage_create_surfaces_storage_errors") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_custom_storage_errors_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    RaftorConfig config;
    config.node_id = 1;
    config.transport_kind = TransportKind::Noop;
    config.data_dir = temp_dir;

    {
        auto storage = std::make_shared<FaultyStorage>();
        storage->fail_initial_state = true;
        auto result = Raftor::Create(
            config, std::make_unique<MockStateMachine>(), storage,
            std::make_unique<RecordingTransport>()
        );
        CHECK(!result.has_value());
    }

    {
        auto storage = std::make_shared<FaultyStorage>();
        storage->fail_set_conf_state = true;
        auto result = Raftor::Create(
            config, std::make_unique<MockStateMachine>(), storage,
            std::make_unique<RecordingTransport>()
        );
        CHECK(!result.has_value());
    }

    {
        auto storage = std::make_shared<FaultyStorage>();
        storage->fail_local_snapshot = true;
        auto result = Raftor::Create(
            config, std::make_unique<MockStateMachine>(), storage,
            std::make_unique<RecordingTransport>()
        );
        CHECK(!result.has_value());
    }

    {
        auto storage = std::make_shared<FaultyStorage>();
        Snapshot snapshot = capnp_util::make<msg::Snapshot>();
        auto snapshot_builder = capnp_util::builder<msg::Snapshot>(snapshot);
        snapshot_builder.initMetadata().setIndex(0);
        storage->local_snapshot = std::move(snapshot);

        auto state_machine = std::make_unique<MockStateMachine>();
        auto* state_machine_ptr = state_machine.get();
        auto result = Raftor::Create(
            config, std::move(state_machine), storage, std::make_unique<RecordingTransport>()
        );
        REQUIRE(result.has_value());
        CHECK(state_machine_ptr->RestoreCount() == 0);
    }

    {
        auto storage = std::make_shared<FaultyStorage>();
        RaftorConfig missing_self = config;
        missing_self.initial_peers = {PeerConfig{2, "127.0.0.1:19602"}};
        auto result = Raftor::Create(
            missing_self, std::make_unique<MockStateMachine>(), storage,
            std::make_unique<RecordingTransport>()
        );
        CHECK(!result.has_value());
    }
}

TEST_CASE("default_wal_create_surfaces_bootstrap_errors") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_wal_bootstrap_errors_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    RaftorConfig config;
    config.node_id = 1;
    config.transport_kind = TransportKind::Noop;
    config.data_dir = temp_dir;
    config.initial_peers = {PeerConfig{2, "127.0.0.1:19612"}};

    auto result = Raftor::Create(config, std::make_unique<MockStateMachine>());
    CHECK(!result.has_value());
}

TEST_CASE("custom_writable_storage_restores_initial_transport_peers") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_custom_storage_peers_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    RaftorConfig config;
    config.node_id = 1;
    config.transport_kind = TransportKind::Noop;
    config.data_dir = temp_dir;
    config.initial_peers = {
        PeerConfig{1, "127.0.0.1:19621"},
        PeerConfig{2, "127.0.0.1:19622"},
    };

    auto storage = std::make_shared<FaultyStorage>();
    auto transport = std::make_unique<RecordingTransport>();
    auto* recording_transport = transport.get();
    auto result =
        Raftor::Create(config, std::make_unique<MockStateMachine>(), storage, std::move(transport));

    REQUIRE(result.has_value());
    auto peers = recording_transport->Peers();
    REQUIRE(peers.size() == 1);
    CHECK(peers[0].id == 2);
    CHECK(peers[0].addr == "127.0.0.1:19622");
}

TEST_CASE("memory_storage_default_writable_storage_methods") {
    std::shared_ptr<WritableStorage> storage = std::make_shared<MemoryStorage>();

    CHECK(storage->Sync().has_value());
    CHECK(storage->LogSizeBytes() == 0);
}

TEST_CASE("ready_processor_surfaces_storage_persistence_errors") {
    const auto pid = static_cast<uint64_t>(::getpid());
    auto temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_ready_storage_errors_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir);
    TempDirCleanup cleanup(temp_dir);

    auto make_config = [&](std::string_view suffix) {
        RaftorConfig config;
        config.node_id = 1;
        config.transport_kind = TransportKind::Noop;
        config.data_dir = temp_dir / std::string(suffix);
        config.election_tick = 5;
        config.heartbeat_tick = 1;
        config.tick_interval = 1ms;
        return config;
    };

    {
        auto storage = std::make_shared<FaultyStorage>();
        auto result = Raftor::Create(
            make_config("hard_state"), std::make_unique<MockStateMachine>(), storage,
            std::make_unique<RecordingTransport>()
        );
        REQUIRE(result.has_value());
        auto raftor = std::move(*result);
        REQUIRE(raftor->Start());
        storage->fail_set_hard_state = true;
        REQUIRE(raftor->Campaign());
        raftor->Poll(1ms);
    }

    {
        auto storage = std::make_shared<FaultyStorage>();
        auto result = Raftor::Create(
            make_config("sync"), std::make_unique<MockStateMachine>(), storage,
            std::make_unique<RecordingTransport>()
        );
        REQUIRE(result.has_value());
        auto raftor = std::move(*result);
        REQUIRE(raftor->Start());
        storage->fail_sync = true;
        REQUIRE(raftor->Campaign());
        raftor->Poll(1ms);
    }

    {
        auto storage = std::make_shared<FaultyStorage>();
        auto result = Raftor::Create(
            make_config("conf_state"), std::make_unique<MockStateMachine>(), storage,
            std::make_unique<RecordingTransport>()
        );
        REQUIRE(result.has_value());
        auto raftor = std::move(*result);
        REQUIRE(raftor->Start());
        REQUIRE(raftor->Campaign());

        auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!raftor->IsLeader() && std::chrono::steady_clock::now() < deadline) {
            raftor->Poll(1ms);
        }
        REQUIRE(raftor->IsLeader());

        storage->fail_set_conf_state = true;
        REQUIRE(raftor->AddNode(2, "127.0.0.1:19632"));
        deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline) {
            raftor->Poll(1ms);
        }
    }
}

TEST_CASE("local_snapshot_restored_on_restart") {
    TestNode node;
    const auto pid = static_cast<uint64_t>(::getpid());
    node.temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_snapshot_restart_test_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(node.temp_dir, ec);
    std::filesystem::create_directories(node.temp_dir);
    node.temp_dir_cleanup = std::make_unique<TempDirCleanup>(node.temp_dir);

    node.config.node_id = 1;
    node.config.transport_kind = TransportKind::Noop;
    node.config.data_dir = node.temp_dir;
    node.config.election_tick = 5;
    node.config.heartbeat_tick = 1;
    node.config.tick_interval = 1ms;

    auto state_machine = std::make_unique<MockStateMachine>();
    auto* first_state_machine = state_machine.get();
    auto raftor_result = Raftor::Create(node.config, std::move(state_machine));
    REQUIRE(raftor_result.has_value());

    auto raftor = std::move(*raftor_result);
    REQUIRE(raftor->Start());
    REQUIRE(raftor->Campaign());

    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!raftor->IsLeader() && std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(1ms);
    }
    REQUIRE(raftor->IsLeader());

    std::atomic<bool> proposal_done{false};
    std::atomic<bool> proposal_ok{false};
    raftor->Propose("value", [&](Result<std::string> result) {
        proposal_ok = result.has_value();
        proposal_done = true;
    });

    deadline = std::chrono::steady_clock::now() + 1s;
    while (!proposal_done && std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(1ms);
    }
    REQUIRE(proposal_done.load());
    REQUIRE(proposal_ok.load());
    REQUIRE(first_state_machine->ApplyCount() > 0);

    REQUIRE(raftor->TakeSnapshot());
    const uint64_t snapshot_index = raftor->GetStatus().applied_index;
    REQUIRE(snapshot_index > 0);

    raftor->Stop();
    raftor.reset();

    auto failing_state_machine = std::make_unique<MockStateMachine>();
    failing_state_machine->SetFailRestore(true);
    auto failed_restart = Raftor::Create(node.config, std::move(failing_state_machine));
    CHECK(!failed_restart.has_value());

    auto restarted_state_machine = std::make_unique<MockStateMachine>();
    auto* restored_state_machine = restarted_state_machine.get();
    auto restarted = Raftor::Create(node.config, std::move(restarted_state_machine));
    REQUIRE(restarted.has_value());

    CHECK(restored_state_machine->RestoreCount() == 1);
    CHECK(restored_state_machine->LastRestoredIndex() == snapshot_index);
    CHECK(restored_state_machine->LastRestoredData() == std::vector<uint8_t>({'s', 'n', 'a', 'p'}));
    CHECK((*restarted)->GetStatus().applied_index == snapshot_index);
}

TEST_CASE("three_node_proposal_after_bootstrap") {
    std::vector<PeerConfig> peers = {
        PeerConfig{1, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        PeerConfig{2, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        PeerConfig{3, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
    };

    std::vector<TestNode> test_nodes;
    test_nodes.reserve(peers.size());

    std::vector<std::unique_ptr<Raftor>> raftors;
    raftors.reserve(peers.size());

    for (const auto& peer : peers) {
        auto result = CreateTestNode(peer.id, peer.addr, peers);
        REQUIRE(result.has_value());
        test_nodes.push_back(std::move(*result));
    }

    for (auto& node : test_nodes) {
        auto start_result = node.raftor->Start();
        REQUIRE(start_result.has_value());
        raftors.push_back(std::move(node.raftor));
    }

    PollAll(raftors, 500ms);

    REQUIRE(HasLeader(raftors));
    uint64_t leader_id = GetLeaderId(raftors);
    REQUIRE_NE(leader_id, 0);

    std::promise<bool> proposal_completed;
    auto proposal_future = proposal_completed.get_future();

    std::string test_data = "test proposal data";

    raftors[leader_id - 1]->Propose(test_data, [&proposal_completed](Result<std::string> result) {
        proposal_completed.set_value(result.has_value());
    });

    PollAll(raftors, 300ms);

    REQUIRE(proposal_future.wait_for(100ms) == std::future_status::ready);
    REQUIRE(proposal_future.get());

    PollAll(raftors, 200ms);

    size_t total_applied = 0;
    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        total_applied += status.applied_index;
    }
    REQUIRE_MESSAGE(
        total_applied >= 3,
        "All nodes should have applied at least the no-op entry and our proposal"
    );
}

TEST_CASE("three_node_read_index_from_follower_completes") {
    std::vector<PeerConfig> peers = {
        PeerConfig{1, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        PeerConfig{2, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        PeerConfig{3, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
    };

    std::vector<TestNode> test_nodes;
    test_nodes.reserve(peers.size());

    std::vector<std::unique_ptr<Raftor>> raftors;
    raftors.reserve(peers.size());

    for (auto peer : peers) {
        auto result = CreateTestNode(peer.id, peer.addr, peers);
        REQUIRE(result.has_value());
        auto start_result = result->raftor->Start();
        REQUIRE(start_result.has_value());
        raftors.push_back(std::move(result->raftor));
        test_nodes.push_back(std::move(*result));
    }

    REQUIRE_MESSAGE(WaitForStableLeader(raftors, 2s), "No leader was elected");
    const uint64_t leader_id = GetLeaderId(raftors);
    REQUIRE_NE(leader_id, 0);

    // Ensure there is a committed entry in the current term.
    std::promise<bool> proposal_completed;
    auto proposal_future = proposal_completed.get_future();
    raftors[leader_id - 1]->Propose(
        "readindex_warmup",
        [&proposal_completed](Result<std::string> r) {
            proposal_completed.set_value(r.has_value());
        }
    );
    PollAll(raftors, 500ms);
    REQUIRE(proposal_future.wait_for(100ms) == std::future_status::ready);
    REQUIRE(proposal_future.get());

    // Pick a follower (non-leader) to issue ReadIndex.
    size_t follower_idx = 0;
    bool found_follower = false;
    for (size_t i = 0; i < raftors.size(); ++i) {
        if (raftors[i]->GetStatus().role != StateRole::Leader) {
            follower_idx = i;
            found_follower = true;
            break;
        }
    }
    REQUIRE_MESSAGE(found_follower, "Expected at least one follower");

    auto promise = std::make_shared<std::promise<Result<void>>>();
    auto future = promise->get_future();
    auto completed = std::make_shared<std::atomic<bool>>(false);

    std::string ctx =
        "readindex_from_follower:" + std::to_string(raftors[follower_idx]->GetStatus().id);
    raftors[follower_idx]->ReadIndex(std::move(ctx), [promise, completed](Result<void> r) {
        if (completed->exchange(true)) {
            return;
        }
        promise->set_value(std::move(r));
    });

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (future.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        PollAll(raftors, 25ms);
    }

    REQUIRE_MESSAGE(
        future.wait_for(0ms) == std::future_status::ready, "ReadIndex did not complete"
    );
    const auto result = future.get();
    REQUIRE_MESSAGE(result.has_value(), "ReadIndex failed: {}", result.error().ToString());
}

TEST_CASE("three_node_leader_failure") {
    std::vector<PeerConfig> peers = {
        PeerConfig{1, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        PeerConfig{2, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        PeerConfig{3, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
    };

    std::vector<TestNode> test_nodes;
    test_nodes.reserve(peers.size());

    std::vector<std::unique_ptr<Raftor>> raftors;
    raftors.reserve(peers.size());

    for (const auto& peer : peers) {
        auto result = CreateTestNode(peer.id, peer.addr, peers);
        REQUIRE(result.has_value());
        test_nodes.push_back(std::move(*result));
    }

    for (auto& node : test_nodes) {
        auto start_result = node.raftor->Start();
        REQUIRE(start_result.has_value());
        raftors.push_back(std::move(node.raftor));
    }

    PollAll(raftors, 500ms);

    REQUIRE(HasLeader(raftors));
    uint64_t first_leader_id = GetLeaderId(raftors);

    raftors[first_leader_id - 1]->Stop();

    std::vector<std::unique_ptr<Raftor>> remaining_raftors;
    for (size_t i = 0; i < raftors.size(); ++i) {
        if (i != first_leader_id - 1) {
            remaining_raftors.push_back(std::move(raftors[i]));
        }
    }

    REQUIRE(WaitForStableLeader(remaining_raftors, 2s));
    uint64_t second_leader_id = GetLeaderId(remaining_raftors);

    REQUIRE_MESSAGE(second_leader_id != first_leader_id, "A new leader should be elected");
    REQUIRE_MESSAGE(second_leader_id != 0, "Leader ID should not be zero");
}

TEST_CASE("five_node_cluster_propose") {
    RAFTPP_LOG_INFO("Creating 5-node cluster...");

    std::vector<PeerConfig> peers;
    for (uint64_t i = 1; i <= 5; ++i) {
        peers.push_back(PeerConfig{i, "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())});
    }

    RAFTPP_LOG_INFO("Peers configured:");
    for (const auto& peer : peers) {
        RAFTPP_LOG_INFO("  ID: {}, addr: {}", peer.id, peer.addr);
    }

    std::vector<TestNode> test_nodes;
    test_nodes.reserve(peers.size());

    std::vector<MockStateMachine*> state_machines;
    state_machines.reserve(peers.size());

    for (const auto& peer : peers) {
        auto result = CreateTestNode(peer.id, peer.addr, peers);
        REQUIRE(result.has_value());
        state_machines.push_back(result->state_machine);
        test_nodes.push_back(std::move(*result));
    }

    std::vector<std::unique_ptr<Raftor>> raftors;
    raftors.reserve(test_nodes.size());

    for (auto& node : test_nodes) {
        auto start_result = node.raftor->Start();
        REQUIRE(start_result.has_value());
        raftors.push_back(std::move(node.raftor));
    }

    // Wait for leader election (5 nodes need more time)
    RAFTPP_LOG_INFO("Polling for leader election (up to 2s)...");
    REQUIRE_MESSAGE(WaitForStableLeader(raftors, 2s), "Leader should be elected in 5-node cluster");
    uint64_t leader_id = GetLeaderId(raftors);
    REQUIRE_NE(leader_id, 0);
    REQUIRE_MESSAGE(leader_id <= state_machines.size(), "Leader ID out of range");

    RAFTPP_LOG_INFO("Leader elected: node {}", leader_id);

    // Print cluster status
    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        RAFTPP_LOG_INFO(
            "Node {}: role={}, term={}, leader_id={}", status.id, static_cast<int>(status.role),
            status.term, status.leader_id
        );
    }

    // Verify exactly one leader
    int leader_count = 0;
    for (const auto& r : raftors) {
        if (r->GetStatus().role == StateRole::Leader) {
            leader_count++;
        }
    }
    REQUIRE_MESSAGE(leader_count == 1, "Exactly one leader should exist");

    // Test multiple proposals
    constexpr int kNumProposals = 5;
    std::atomic<int> completed_count{0};
    std::atomic<int> success_count{0};

    RAFTPP_LOG_INFO("Submitting {} proposals...", kNumProposals);

    for (int i = 0; i < kNumProposals; ++i) {
        std::string data = "proposal_" + std::to_string(i);
        raftors[leader_id - 1]->Propose(data, [&, i](Result<std::string> result) {
            completed_count++;
            if (result.has_value()) {
                success_count++;
                RAFTPP_LOG_INFO("Proposal {} succeeded: {}", i, *result);
            } else {
                RAFTPP_LOG_WARN("Proposal {} failed: {}", i, result.error().ToString());
            }
        });
    }

    // Poll until all proposals complete or timeout
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (completed_count < kNumProposals && std::chrono::steady_clock::now() < deadline) {
        for (auto& r : raftors) {
            r->Poll(1ms);
        }
        std::this_thread::sleep_for(1ms);
    }

    RAFTPP_LOG_INFO(
        "Completed: {}/{}, Success: {}", completed_count.load(), kNumProposals, success_count.load()
    );

    REQUIRE_MESSAGE(completed_count == kNumProposals, "All proposals should complete");
    REQUIRE_MESSAGE(success_count == kNumProposals, "All proposals should succeed");

    // Allow time for replication to all nodes
    // Need more time for commit to propagate via heartbeats
    PollAll(raftors, 1000ms);

    auto apply_deadline = std::chrono::steady_clock::now() + 1s;
    while (state_machines[leader_id - 1]->ApplyCount() < static_cast<size_t>(kNumProposals + 1) &&
           std::chrono::steady_clock::now() < apply_deadline) {
        PollAll(raftors, 50ms);
    }

    // Verify the leader has applied all entries
    RAFTPP_LOG_INFO("Checking applied indices...");
    auto leader_status = raftors[leader_id - 1]->GetStatus();
    RAFTPP_LOG_INFO(
        "Leader (Node {}): applied_index={}, commit_index={}", leader_status.id,
        leader_status.applied_index, leader_status.commit_index
    );

    // Leader should have applied the no-op entry + all our proposals
    REQUIRE_MESSAGE(
        leader_status.applied_index >= static_cast<uint64_t>(kNumProposals + 1),
        "Leader should have applied at least {} entries (no-op + proposals), got {}",
        kNumProposals + 1, leader_status.applied_index
    );

    auto leader_entries = state_machines[leader_id - 1]->GetAppliedEntries();
    for (int i = 0; i < kNumProposals; ++i) {
        std::string expected = "proposal_" + std::to_string(i);
        bool found = false;
        for (const auto& entry : leader_entries) {
            if (std::string(entry.begin(), entry.end()) == expected) {
                found = true;
                break;
            }
        }
        REQUIRE_MESSAGE(found, "Leader state machine should apply {}", expected);
    }

    // Check all nodes and count how many have caught up
    int caught_up_count = 0;
    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        RAFTPP_LOG_INFO(
            "Node {}: applied_index={}, commit_index={}", status.id, status.applied_index,
            status.commit_index
        );
        if (status.applied_index >= static_cast<uint64_t>(kNumProposals)) {
            caught_up_count++;
        }
    }

    // At least a majority (3 out of 5) should have applied the entries
    REQUIRE_MESSAGE(
        caught_up_count >= 3,
        "At least 3 nodes should have applied {} entries, got {} nodes caught up", kNumProposals,
        caught_up_count
    );
}

TEST_SUITE_END();
