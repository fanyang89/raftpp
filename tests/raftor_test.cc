#include "raftpp/raftor/raftor.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include <doctest/doctest.h>

#include "raftpp/core/capnp_util.h"
#include "raftpp/core/memory_storage.h"
#include "raftpp/raftor/proposal_tracker.h"
#include "raftpp/raftor/raftor_config.h"
#include "raftpp/raftor/rpc/transport.h"
#include "raftpp/raftor/state_machine.h"

using namespace raftpp;
using namespace raftpp::raftor;
using namespace std::chrono_literals;

namespace {

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

    Result<SnapshotData> TakeSnapshot(
        uint64_t applied_index, uint64_t applied_term, const ConfState& conf_state
    ) override {
        std::lock_guard lock(mutex_);
        SnapshotData data;
        data.data = {'s', 'n', 'a', 'p'};
        return data;
    }

    Result<void> RestoreSnapshot(const SnapshotData& snapshot) override {
        std::lock_guard lock(mutex_);
        restore_count_++;
        return {};
    }

    void OnLeadershipChange(bool is_leader, uint64_t term, uint64_t leader_id) override {
        std::lock_guard lock(mutex_);
        leadership_changes_.push_back({is_leader, term, leader_id});
        std::cout << "Leadership change: is_leader=" << is_leader << ", term=" << term
                  << ", leader_id=" << leader_id << std::endl;
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
};

Result<TestNode> CreateTestNode(
    uint64_t node_id, const std::string& listen_addr, const std::vector<PeerConfig>& initial_peers
) {
    TestNode node;

    node.temp_dir = std::filesystem::temp_directory_path() /
        ("raftpp_test_" + std::to_string(node_id) + "_" + std::to_string(std::time(nullptr)));
    std::filesystem::create_directories(node.temp_dir);

    node.config.node_id = node_id;
    node.config.listen_addr = listen_addr;
    node.config.data_dir = node.temp_dir;
    node.config.election_tick = 3;
    node.config.heartbeat_tick = 1;
    node.config.tick_interval = 10ms;
    node.config.pre_vote = true;
    node.config.check_quorum = true;
    node.config.initial_peers = initial_peers;

    auto state_machine = std::make_unique<MockStateMachine>();
    node.state_machine = state_machine.get();

    auto raftor_result = Raftor::Create(node.config, std::move(state_machine));
    if (!raftor_result) {
        return std::unexpected(raftor_result.error());
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
    std::cout << "Creating 3-node cluster..." << std::endl;

    std::vector<PeerConfig> peers = {
        {.id = 1, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 2, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 3, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
    };

    std::cout << "Peers configured:" << std::endl;
    for (const auto& peer : peers) {
        std::cout << "  ID: " << peer.id << ", addr: " << peer.addr << std::endl;
    }

    std::vector<TestNode> test_nodes;

    for (auto peer : peers) {
        auto result = CreateTestNode(peer.id, peer.addr, peers);
        if (!result) {
            std::cout << "Error creating node " << peer.id << ": " << result.error().ToString()
                      << std::endl;
        }
        REQUIRE(result.has_value());
        test_nodes.push_back(std::move(*result));
    }

    std::vector<std::unique_ptr<Raftor>> raftors;
    for (auto& node : test_nodes) {
        auto start_result = node.raftor->Start();
        if (!start_result) {
            std::cout << "Error starting node: " << start_result.error().ToString() << std::endl;
        }
        REQUIRE(start_result.has_value());
        raftors.push_back(std::move(node.raftor));
    }

    std::cout << "Polling for 500ms..." << std::endl;
    PollAll(raftors, 500ms);

    for (size_t i = 0; i < raftors.size(); ++i) {
        auto status = raftors[i]->GetStatus();
        std::cout << "Node " << status.id << ": role=" << static_cast<int>(status.role)
                  << ", term=" << status.term << ", leader_id=" << status.leader_id << std::endl;
    }

    bool leader_found = false;
    uint64_t leader_id = 0;
    for (size_t i = 0; i < raftors.size(); ++i) {
        auto status = raftors[i]->GetStatus();
        if (status.role == StateRole::Leader) {
            leader_found = true;
            leader_id = status.id;
            break;
        }
    }
    REQUIRE_MESSAGE(leader_found, "No leader was elected");
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

    std::vector<std::unique_ptr<Raftor>> raftors;

    for (auto peer : peers) {
        auto result = CreateTestNode(peer.id, peer.addr, peers);
        REQUIRE(result.has_value());
        result->raftor->Start();
        raftors.push_back(std::move(result->raftor));
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

TEST_CASE("three_node_leader_failure") {
    std::vector<PeerConfig> peers = {
        {.id = 1, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 2, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
        {.id = 3, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())},
    };

    std::vector<std::unique_ptr<Raftor>> raftors;

    for (auto peer : peers) {
        auto result = CreateTestNode(peer.id, peer.addr, peers);
        REQUIRE(result.has_value());
        result->raftor->Start();
        raftors.push_back(std::move(result->raftor));
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

    PollAll(remaining_raftors, 800ms);

    REQUIRE(HasLeader(remaining_raftors));
    uint64_t second_leader_id = GetLeaderId(remaining_raftors);

    REQUIRE_MESSAGE(second_leader_id != first_leader_id, "A new leader should be elected");
    REQUIRE_MESSAGE(second_leader_id != 0, "Leader ID should not be zero");
}

TEST_CASE("five_node_cluster_propose") {
    std::cout << "Creating 5-node cluster..." << std::endl;

    std::vector<PeerConfig> peers;
    for (uint64_t i = 1; i <= 5; ++i) {
        peers.push_back({.id = i, .addr = "127.0.0.1:" + std::to_string(PortAllocator::GetNextPort())}
        );
    }

    std::cout << "Peers configured:" << std::endl;
    for (const auto& peer : peers) {
        std::cout << "  ID: " << peer.id << ", addr: " << peer.addr << std::endl;
    }

    std::vector<std::unique_ptr<Raftor>> raftors;
    std::vector<MockStateMachine*> state_machines;
    state_machines.reserve(peers.size());

    for (const auto& peer : peers) {
        auto result = CreateTestNode(peer.id, peer.addr, peers);
        REQUIRE(result.has_value());
        state_machines.push_back(result->state_machine);
        auto start_result = result->raftor->Start();
        REQUIRE(start_result.has_value());
        raftors.push_back(std::move(result->raftor));
    }

    // Wait for leader election (5 nodes need more time)
    std::cout << "Polling for leader election (600ms)..." << std::endl;
    PollAll(raftors, 600ms);

    REQUIRE_MESSAGE(HasLeader(raftors), "Leader should be elected in 5-node cluster");
    uint64_t leader_id = GetLeaderId(raftors);
    REQUIRE_NE(leader_id, 0);
    REQUIRE_MESSAGE(leader_id <= state_machines.size(), "Leader ID out of range");

    std::cout << "Leader elected: node " << leader_id << std::endl;

    // Print cluster status
    for (const auto& r : raftors) {
        auto status = r->GetStatus();
        std::cout << "Node " << status.id << ": role=" << static_cast<int>(status.role)
                  << ", term=" << status.term << ", leader_id=" << status.leader_id << std::endl;
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

    std::cout << "Submitting " << kNumProposals << " proposals..." << std::endl;

    for (int i = 0; i < kNumProposals; ++i) {
        std::string data = "proposal_" + std::to_string(i);
        raftors[leader_id - 1]->Propose(data, [&, i](Result<std::string> result) {
            completed_count++;
            if (result.has_value()) {
                success_count++;
                std::cout << "Proposal " << i << " succeeded: " << *result << std::endl;
            } else {
                std::cout << "Proposal " << i << " failed: " << result.error().ToString()
                          << std::endl;
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

    std::cout << "Completed: " << completed_count << "/" << kNumProposals
              << ", Success: " << success_count << std::endl;

    REQUIRE_MESSAGE(completed_count == kNumProposals, "All proposals should complete");
    REQUIRE_MESSAGE(success_count == kNumProposals, "All proposals should succeed");

    // Allow time for replication to all nodes
    // Need more time for commit to propagate via heartbeats
    PollAll(raftors, 1000ms);

    auto apply_deadline = std::chrono::steady_clock::now() + 1s;
    while (state_machines[leader_id - 1]->ApplyCount() <
               static_cast<size_t>(kNumProposals + 1) &&
           std::chrono::steady_clock::now() < apply_deadline) {
        PollAll(raftors, 50ms);
    }

    // Verify the leader has applied all entries
    std::cout << "Checking applied indices..." << std::endl;
    auto leader_status = raftors[leader_id - 1]->GetStatus();
    std::cout << "Leader (Node " << leader_status.id << "): applied_index=" << leader_status.applied_index
              << ", commit_index=" << leader_status.commit_index << std::endl;

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
        std::cout << "Node " << status.id << ": applied_index=" << status.applied_index
                  << ", commit_index=" << status.commit_index << std::endl;
        if (status.applied_index >= static_cast<uint64_t>(kNumProposals)) {
            caught_up_count++;
        }
    }

    // At least a majority (3 out of 5) should have applied the entries
    REQUIRE_MESSAGE(
        caught_up_count >= 3,
        "At least 3 nodes should have applied {} entries, got {} nodes caught up",
        kNumProposals, caught_up_count
    );
}

TEST_SUITE_END();
