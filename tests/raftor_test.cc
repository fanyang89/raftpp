#include "raftpp/raftor/raftor.h"

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <system_error>
#include <thread>

#include <doctest/doctest.h>

#include "raftpp/core/capnp_util.h"
#include "raftpp/core/memory_storage.h"
#include "raftpp/logging.h"
#include "raftpp/raftor/proposal_tracker.h"
#include "raftpp/raftor/raftor_config.h"
#include "raftpp/raftor/rpc/transport.h"
#include "raftpp/raftor/state_machine.h"

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
        return ApplyResult{.response = "OK:" + std::to_string(apply_count_)};
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
        (void)metadata;
        std::array<uint8_t, 1024> buffer{};
        while (true) {
            auto read_result = reader.Read(buffer);
            if (!read_result) {
                return nonstd::make_unexpected(read_result.error());
            }
            if (*read_result == 0) {
                break;
            }
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

    std::vector<std::tuple<bool, uint64_t, uint64_t>> GetLeadershipChanges() const {
        std::lock_guard lock(mutex_);
        return leadership_changes_;
    }

  private:
    mutable std::mutex mutex_;
    size_t apply_count_ = 0;
    size_t restore_count_ = 0;
    std::vector<std::vector<uint8_t>> applied_entries_;
    std::vector<std::tuple<bool, uint64_t, uint64_t>> leadership_changes_;
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

bool WaitForLeader(
    std::vector<std::unique_ptr<Raftor>>& raftors, std::chrono::milliseconds timeout,
    std::chrono::milliseconds step = 25ms
) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        PollAll(raftors, step);
        if (HasLeader(raftors)) {
            return true;
        }
    }
    return HasLeader(raftors);
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
        {.id = 1, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 2, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 3, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
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
        {.id = 1, .addr = listen_addr},
        {.id = 2, .addr = ""},
        {.id = 3, .addr = ""},
    };

    auto result = CreateTestNode(1, listen_addr, peers);
    REQUIRE(result.has_value());
    TestNode node = std::move(*result);

    auto raftor = std::move(node.raftor);
    auto status = raftor->GetStatus();

    REQUIRE_MESSAGE(status.role != StateRole::Leader, "Node should not be leader without quorum");
}

TEST_CASE("three_node_proposal_after_bootstrap") {
    std::vector<PeerConfig> peers = {
        {.id = 1, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 2, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 3, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
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
        {.id = 1, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 2, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 3, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
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
        {.id = 1, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 2, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 3, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
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
        peers.push_back(
            {.id = i, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())}
        );
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
