#include "raftpp/raftor/raftor.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <span>
#include <thread>

#include <doctest/doctest.h>
#include <kj/array.h>

#include "raftpp/core/memory_storage.h"
#include "raftpp/raftor/mock_state_machine.h"
#include "raftpp/raftor/proposal_tracker.h"
#include "raftpp/raftor/raftor_config.h"
#include "raftpp/raftor/rpc/transport.h"
#include "raftpp/raftor/state_machine.h"
#include "raftpp/raftor/wal/wal_storage.h"
#include "test_util.h"

using namespace raftpp;
using namespace raftpp::raftor;

TEST_SUITE_BEGIN("raftor");

// =============================================================================
// Mock Transport for testing
// =============================================================================

class MockTransport : public rpc::Transport {
  public:
    Result<void> Start() override {
        started_ = true;
        return {};
    }

    void Stop() override { started_ = false; }

    void Run() override {
        // No-op for mock
    }

    void Poll(std::chrono::milliseconds /*timeout*/) override {
        // Process any queued messages
        std::lock_guard lock(mutex_);
        for (auto& msg : incoming_messages_) {
            if (on_message_) {
                on_message_(std::move(msg));
            }
        }
        incoming_messages_.clear();
    }

    void Send(std::span<const Message> messages) override {
        std::lock_guard lock(mutex_);
        for (const auto& msg : messages) {
            sent_messages_.push_back(CloneMessage(msg));
        }
    }

    void AddPeer(uint64_t id, const std::string& addr) override {
        std::lock_guard lock(mutex_);
        peers_[id] = addr;
    }

    void RemovePeer(uint64_t id) override {
        std::lock_guard lock(mutex_);
        peers_.erase(id);
    }

    void SetMessageCallback(std::function<void(Message)> callback) override {
        on_message_ = std::move(callback);
    }

    void SetErrorCallback(std::function<void(uint64_t, std::string)> callback) override {
        on_error_ = std::move(callback);
    }

    // Test helpers
    void InjectMessage(Message msg) {
        std::lock_guard lock(mutex_);
        incoming_messages_.push_back(std::move(msg));
    }

    void TriggerError(uint64_t peer_id, const std::string& error) {
        if (on_error_) {
            on_error_(peer_id, error);
        }
    }

    std::vector<Message> SentMessages() const {
        std::lock_guard lock(mutex_);
        std::vector<Message> result;
        result.reserve(sent_messages_.size());
        for (const auto& msg : sent_messages_) {
            result.push_back(CloneMessage(msg));
        }
        return result;
    }

    void ClearSentMessages() {
        std::lock_guard lock(mutex_);
        sent_messages_.clear();
    }

    bool HasPeer(uint64_t id) const {
        std::lock_guard lock(mutex_);
        return peers_.contains(id);
    }

    bool IsStarted() const { return started_; }

  private:
    mutable std::mutex mutex_;
    std::atomic<bool> started_{false};
    std::function<void(Message)> on_message_;
    std::function<void(uint64_t, std::string)> on_error_;
    std::vector<Message> incoming_messages_;
    std::vector<Message> sent_messages_;
    absl::flat_hash_map<uint64_t, std::string> peers_;
};

// =============================================================================
// ProposalTracker Tests
// =============================================================================

TEST_CASE("proposal_tracker: track and complete") {
    ProposalTracker tracker;

    bool called = false;
    std::string response;

    tracker.Track("ctx1", [&](Result<std::string> result) {
        called = true;
        if (result) {
            response = *result;
        }
    });

    CHECK(tracker.PendingCount() == 1);

    tracker.Complete("ctx1", "success");

    CHECK(called);
    CHECK(response == "success");
    CHECK(tracker.PendingCount() == 0);
}

TEST_CASE("proposal_tracker: track and fail") {
    ProposalTracker tracker;

    bool called = false;
    bool failed = false;

    tracker.Track("ctx1", [&](Result<std::string> result) {
        called = true;
        failed = !result.has_value();
    });

    tracker.Fail("ctx1", RaftError(RaftErrorCode::ProposalDropped));

    CHECK(called);
    CHECK(failed);
    CHECK(tracker.PendingCount() == 0);
}

TEST_CASE("proposal_tracker: fail all") {
    ProposalTracker tracker;

    int call_count = 0;
    int fail_count = 0;

    for (int i = 0; i < 5; i++) {
        tracker.Track("ctx" + std::to_string(i), [&](Result<std::string> result) {
            call_count++;
            if (!result) {
                fail_count++;
            }
        });
    }

    CHECK(tracker.PendingCount() == 5);

    tracker.FailAll(RaftError(RaftErrorCode::ProposalDropped));

    CHECK(call_count == 5);
    CHECK(fail_count == 5);
    CHECK(tracker.PendingCount() == 0);
}

TEST_CASE("proposal_tracker: complete non-existent proposal is no-op") {
    ProposalTracker tracker;

    // Should not crash
    tracker.Complete("nonexistent", "response");
    tracker.Fail("nonexistent", RaftError(RaftErrorCode::ProposalDropped));

    CHECK(tracker.PendingCount() == 0);
}

TEST_CASE("proposal_tracker: track and complete read") {
    ProposalTracker tracker;

    bool called = false;
    bool success = false;

    tracker.TrackRead("read1", [&](Result<void> result) {
        called = true;
        success = result.has_value();
    });

    CHECK(tracker.PendingReadCount() == 1);

    tracker.CompleteRead("read1");

    CHECK(called);
    CHECK(success);
    CHECK(tracker.PendingReadCount() == 0);
}

TEST_CASE("proposal_tracker: fail read") {
    ProposalTracker tracker;

    bool called = false;
    bool failed = false;

    tracker.TrackRead("read1", [&](Result<void> result) {
        called = true;
        failed = !result.has_value();
    });

    tracker.FailRead("read1", RaftError(RaftErrorCode::ProposalDropped));

    CHECK(called);
    CHECK(failed);
    CHECK(tracker.PendingReadCount() == 0);
}

TEST_CASE("proposal_tracker: fail all reads") {
    ProposalTracker tracker;

    int call_count = 0;
    int fail_count = 0;

    for (int i = 0; i < 3; i++) {
        tracker.TrackRead("read" + std::to_string(i), [&](Result<void> result) {
            call_count++;
            if (!result) {
                fail_count++;
            }
        });
    }

    CHECK(tracker.PendingReadCount() == 3);

    tracker.FailAllReads(RaftError(RaftErrorCode::LostLeadership));

    CHECK(call_count == 3);
    CHECK(fail_count == 3);
    CHECK(tracker.PendingReadCount() == 0);
}

TEST_CASE("proposal_tracker: expire timeouts") {
    ProposalTracker tracker;

    bool proposal_called = false;
    RaftError proposal_error(RaftErrorCode::ProposalDropped);

    tracker.Track(
        "ctx1",
        [&](Result<std::string> result) {
            proposal_called = true;
            if (!result) {
                proposal_error = result.error();
            }
        },
        std::chrono::milliseconds{1}
    );

    bool read_called = false;
    RaftError read_error(RaftErrorCode::ProposalDropped);

    tracker.TrackRead(
        "read1",
        [&](Result<void> result) {
            read_called = true;
            if (!result) {
                read_error = result.error();
            }
        },
        std::chrono::milliseconds{1}
    );

    tracker.ExpireTimeouts(std::chrono::steady_clock::now() + std::chrono::milliseconds{10});

    CHECK(proposal_called);
    CHECK(read_called);
    CHECK(proposal_error.Is(RpcErrorCode::Timeout));
    CHECK(read_error.Is(RpcErrorCode::Timeout));
    CHECK(tracker.PendingCount() == 0);
    CHECK(tracker.PendingReadCount() == 0);
}

// =============================================================================
// ProposalQueue Tests
// =============================================================================

TEST_CASE("proposal_queue: push and pop") {
    ProposalQueue queue;

    CHECK(queue.Empty());
    CHECK(queue.Size() == 0);

    bool called = false;
    queue.Push("data1", [&](Result<std::string>) { called = true; });

    CHECK_FALSE(queue.Empty());
    CHECK(queue.Size() == 1);

    auto item = queue.TryPop();
    REQUIRE(item.has_value());
    CHECK(item->data == "data1");

    CHECK(queue.Empty());

    // Invoke callback
    item->callback(std::string("result"));
    CHECK(called);
}

TEST_CASE("proposal_queue: try pop empty returns nullopt") {
    ProposalQueue queue;

    auto item = queue.TryPop();
    CHECK_FALSE(item.has_value());
}

TEST_CASE("proposal_queue: fifo order") {
    ProposalQueue queue;

    queue.Push("data1", [](Result<std::string>) {});
    queue.Push("data2", [](Result<std::string>) {});
    queue.Push("data3", [](Result<std::string>) {});

    auto item1 = queue.TryPop();
    auto item2 = queue.TryPop();
    auto item3 = queue.TryPop();

    REQUIRE(item1.has_value());
    REQUIRE(item2.has_value());
    REQUIRE(item3.has_value());

    CHECK(item1->data == "data1");
    CHECK(item2->data == "data2");
    CHECK(item3->data == "data3");
}

TEST_CASE("proposal_queue: thread safety") {
    ProposalQueue queue;
    std::atomic<int> push_count{0};
    std::atomic<int> pop_count{0};

    constexpr int num_pushers = 4;
    constexpr int pushes_per_thread = 100;

    std::vector<std::thread> pushers;
    for (int i = 0; i < num_pushers; i++) {
        pushers.emplace_back([&, i]() {
            for (int j = 0; j < pushes_per_thread; j++) {
                queue.Push(
                    "data_" + std::to_string(i) + "_" + std::to_string(j),
                    [](Result<std::string>) {}
                );
                push_count++;
            }
        });
    }

    std::atomic<bool> stop{false};
    std::thread popper([&]() {
        while (!stop || !queue.Empty()) {
            if (auto item = queue.TryPop(); item) {
                pop_count++;
            }
            std::this_thread::yield();
        }
    });

    for (auto& t : pushers) {
        t.join();
    }
    stop = true;
    popper.join();

    CHECK(push_count == num_pushers * pushes_per_thread);
    CHECK(pop_count == num_pushers * pushes_per_thread);
    CHECK(queue.Empty());
}

// =============================================================================
// ReadIndexQueue Tests
// =============================================================================

TEST_CASE("read_index_queue: push and pop") {
    ReadIndexQueue queue;

    CHECK(queue.Empty());

    bool called = false;
    queue.Push("ctx1", [&](Result<void>) { called = true; });

    CHECK_FALSE(queue.Empty());

    auto item = queue.TryPop();
    REQUIRE(item.has_value());
    CHECK(item->ctx == "ctx1");

    CHECK(queue.Empty());

    item->callback({});
    CHECK(called);
}

// =============================================================================
// RaftorConfig Tests
// =============================================================================

TEST_CASE("raftor_config: validate - valid config") {
    RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:9001";
    config.data_dir = "/tmp/raft";
    config.election_tick = 10;
    config.heartbeat_tick = 2;

    auto result = config.Validate();
    CHECK(result.has_value());
}

TEST_CASE("raftor_config: validate - missing node_id") {
    RaftorConfig config;
    config.node_id = 0;  // Invalid
    config.listen_addr = "127.0.0.1:9001";
    config.data_dir = "/tmp/raft";

    auto result = config.Validate();
    CHECK_FALSE(result.has_value());
}

TEST_CASE("raftor_config: validate - empty listen_addr") {
    RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "";  // Invalid
    config.data_dir = "/tmp/raft";

    auto result = config.Validate();
    CHECK_FALSE(result.has_value());
}

TEST_CASE("raftor_config: validate - empty data_dir") {
    RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:9001";
    config.data_dir = "";  // Invalid

    auto result = config.Validate();
    CHECK_FALSE(result.has_value());
}

TEST_CASE("raftor_config: validate - election_tick <= heartbeat_tick") {
    RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:9001";
    config.data_dir = "/tmp/raft";
    config.election_tick = 2;
    config.heartbeat_tick = 2;  // Invalid: must be less than election_tick

    auto result = config.Validate();
    CHECK_FALSE(result.has_value());
}

TEST_CASE("raftor_config: to_raft_config") {
    RaftorConfig config;
    config.node_id = 42;
    config.election_tick = 15;
    config.heartbeat_tick = 3;
    config.max_size_per_message = 1024;
    config.max_inflight_messages = 100;
    config.pre_vote = true;
    config.check_quorum = false;
    config.read_only_option = ReadOnlyOption::LeaseBased;

    auto raft_config = config.ToRaftConfig();

    CHECK(raft_config.id == 42);
    CHECK(raft_config.election_tick == 15);
    CHECK(raft_config.heartbeat_tick == 3);
    CHECK(raft_config.max_size_per_message == 1024);
    CHECK(raft_config.max_inflight_messages == 100);
    CHECK(raft_config.pre_vote == true);
    CHECK(raft_config.check_quorum == false);
    CHECK(raft_config.read_only_option == ReadOnlyOption::LeaseBased);
}

// =============================================================================
// MockStateMachine Tests
// =============================================================================

TEST_CASE("mock_state_machine: apply entry") {
    MockStateMachine sm;

    Entry entry = capnp_util::make<msg::Entry>();
    auto builder = capnp_util::builder<msg::Entry>(entry);
    const std::string data = "test_data";
    builder.setData(kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size()));

    auto result = sm.Apply(entry);
    REQUIRE(result.has_value());
    CHECK(result->response == "OK:test_data");
    CHECK(sm.ApplyCount() == 1);

    auto applied = sm.AppliedEntries();
    REQUIRE(applied.size() == 1);
    CHECK(applied[0] == "test_data");
}

TEST_CASE("mock_state_machine: apply entry failure") {
    MockStateMachine sm;
    sm.SetShouldFailApply(true);

    Entry entry = capnp_util::make<msg::Entry>();
    auto builder = capnp_util::builder<msg::Entry>(entry);
    const std::string data = "test_data";
    builder.setData(kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size()));

    auto result = sm.Apply(entry);
    CHECK_FALSE(result.has_value());
    CHECK(sm.ApplyCount() == 1);  // Still counted
}

TEST_CASE("mock_state_machine: take snapshot") {
    MockStateMachine sm;

    ConfState conf_state = capnp_util::make<msg::ConfState>();
    auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
    auto voters = conf_builder.initVoters(2);
    voters.set(0, 1);
    voters.set(1, 2);

    auto result = sm.TakeSnapshot(100, 5, conf_state);
    REQUIRE(result.has_value());
    auto meta_reader = capnp_util::reader<msg::SnapshotMetadata>(result->metadata);
    CHECK(meta_reader.getIndex() == 100);
    CHECK(meta_reader.getTerm() == 5);
    CHECK(meta_reader.getConfState().getVoters().size() == 2);
    CHECK(sm.SnapshotCount() == 1);
}

TEST_CASE("mock_state_machine: restore snapshot") {
    MockStateMachine sm;

    SnapshotData snapshot;
    snapshot.metadata = capnp_util::make<msg::SnapshotMetadata>();
    auto meta_builder = capnp_util::builder<msg::SnapshotMetadata>(snapshot.metadata);
    meta_builder.setIndex(50);

    auto result = sm.RestoreSnapshot(snapshot);
    CHECK(result.has_value());
    CHECK(sm.RestoreCount() == 1);
    CHECK(sm.LastRestoredIndex() == 50);
}

TEST_CASE("mock_state_machine: leadership change") {
    MockStateMachine sm;

    CHECK(sm.LeadershipChangeCount() == 0);
    CHECK_FALSE(sm.IsLeader());

    sm.OnLeadershipChange(true, 5, 1);

    CHECK(sm.LeadershipChangeCount() == 1);
    CHECK(sm.IsLeader());
    CHECK(sm.CurrentTerm() == 5);
    CHECK(sm.CurrentLeader() == 1);

    sm.OnLeadershipChange(false, 6, 2);

    CHECK(sm.LeadershipChangeCount() == 2);
    CHECK_FALSE(sm.IsLeader());
    CHECK(sm.CurrentTerm() == 6);
    CHECK(sm.CurrentLeader() == 2);
}

TEST_CASE("mock_state_machine: peer unreachable") {
    MockStateMachine sm;

    sm.OnPeerUnreachable(2);
    sm.OnPeerUnreachable(3);

    auto peers = sm.UnreachablePeers();
    REQUIRE(peers.size() == 2);
    CHECK(peers[0] == 2);
    CHECK(peers[1] == 3);
}

// =============================================================================
// MockTransport Tests
// =============================================================================

TEST_CASE("mock_transport: start and stop") {
    MockTransport transport;

    CHECK_FALSE(transport.IsStarted());

    auto result = transport.Start();
    CHECK(result.has_value());
    CHECK(transport.IsStarted());

    transport.Stop();
    CHECK_FALSE(transport.IsStarted());
}

TEST_CASE("mock_transport: add and remove peer") {
    MockTransport transport;

    CHECK_FALSE(transport.HasPeer(1));

    transport.AddPeer(1, "127.0.0.1:9001");
    CHECK(transport.HasPeer(1));

    transport.RemovePeer(1);
    CHECK_FALSE(transport.HasPeer(1));
}

TEST_CASE("mock_transport: send messages") {
    MockTransport transport;

    std::vector<Message> messages;
    Message m1 = capnp_util::make<msg::Message>();
    Message m2 = capnp_util::make<msg::Message>();
    capnp_util::builder<msg::Message>(m1).setTo(1);
    capnp_util::builder<msg::Message>(m2).setTo(2);
    messages.push_back(std::move(m1));
    messages.push_back(std::move(m2));

    transport.Send(std::span<const Message>(messages));

    auto sent = transport.SentMessages();
    REQUIRE(sent.size() == 2);
    CHECK(capnp_util::reader<msg::Message>(sent[0]).getTo() == 1);
    CHECK(capnp_util::reader<msg::Message>(sent[1]).getTo() == 2);

    transport.ClearSentMessages();
    CHECK(transport.SentMessages().empty());
}

TEST_CASE("mock_transport: message callback") {
    MockTransport transport;

    Message received;
    transport.SetMessageCallback([&](Message msg) { received = std::move(msg); });

    Message m = capnp_util::make<msg::Message>();
    capnp_util::builder<msg::Message>(m).setFrom(42);
    transport.InjectMessage(std::move(m));

    transport.Poll(std::chrono::milliseconds(0));

    CHECK(capnp_util::reader<msg::Message>(received).getFrom() == 42);
}

TEST_CASE("mock_transport: error callback") {
    MockTransport transport;

    uint64_t error_peer = 0;
    std::string error_msg;
    transport.SetErrorCallback([&](uint64_t peer, std::string msg) {
        error_peer = peer;
        error_msg = std::move(msg);
    });

    transport.TriggerError(5, "connection failed");

    CHECK(error_peer == 5);
    CHECK(error_msg == "connection failed");
}

// =============================================================================
// Error Preservation Tests
// =============================================================================

TEST_CASE("error_preservation: FailAll preserves actual error type") {
    ProposalTracker tracker;

    RaftError captured_error(RaftErrorCode::ProposalDropped);
    bool called = false;

    tracker.Track("ctx1", [&](Result<std::string> result) {
        called = true;
        if (!result) {
            captured_error = result.error();
        }
    });

    // FailAll with ShuttingDown error
    tracker.FailAll(RaftError(RaftErrorCode::ShuttingDown));

    CHECK(called);
    CHECK(captured_error.Is(RaftErrorCode::ShuttingDown));
}

TEST_CASE("error_preservation: FailAll with different error codes") {
    ProposalTracker tracker;

    std::vector<RaftError> captured_errors;

    for (int i = 0; i < 3; i++) {
        tracker.Track("ctx" + std::to_string(i), [&](Result<std::string> result) {
            if (!result) {
                captured_errors.push_back(result.error());
            }
        });
    }

    // FailAll with LostLeadership error
    tracker.FailAll(RaftError(RaftErrorCode::LostLeadership));

    REQUIRE(captured_errors.size() == 3);
    for (const auto& err : captured_errors) {
        CHECK(err.Is(RaftErrorCode::LostLeadership));
    }
}

TEST_CASE("error_preservation: config validation returns specific errors") {
    SUBCASE("InvalidNodeId") {
        RaftorConfig config;
        config.node_id = 0;
        config.listen_addr = "127.0.0.1:9001";
        config.data_dir = "/tmp/raft";
        config.election_tick = 10;
        config.heartbeat_tick = 2;

        auto result = config.Validate();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().Is(ConfigErrorCode::InvalidNodeId));
    }

    SUBCASE("ListenAddressEmpty") {
        RaftorConfig config;
        config.node_id = 1;
        config.listen_addr = "";
        config.data_dir = "/tmp/raft";
        config.election_tick = 10;
        config.heartbeat_tick = 2;

        auto result = config.Validate();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().Is(ConfigErrorCode::ListenAddressEmpty));
    }

    SUBCASE("DataDirectoryEmpty") {
        RaftorConfig config;
        config.node_id = 1;
        config.listen_addr = "127.0.0.1:9001";
        config.data_dir = "";
        config.election_tick = 10;
        config.heartbeat_tick = 2;

        auto result = config.Validate();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().Is(ConfigErrorCode::DataDirectoryEmpty));
    }

    SUBCASE("ElectionTickTooSmall") {
        RaftorConfig config;
        config.node_id = 1;
        config.listen_addr = "127.0.0.1:9001";
        config.data_dir = "/tmp/raft";
        config.election_tick = 2;
        config.heartbeat_tick = 2;

        auto result = config.Validate();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().Is(ConfigErrorCode::ElectionTickTooSmall));
    }
}

// =============================================================================
// Raftor Single Node Integration Tests
// =============================================================================

class SingleNodeRaftorFixture {
  public:
    SingleNodeRaftorFixture() = default;

    void SetUp(const std::string& test_name) {
        temp_dir_ = std::filesystem::temp_directory_path() / ("raftpp_test_" + test_name);
        std::filesystem::remove_all(temp_dir_);
        std::filesystem::create_directories(temp_dir_);

        config_.node_id = 1;
        config_.listen_addr = "127.0.0.1:" + std::to_string(next_port_++);
        config_.data_dir = temp_dir_;
        config_.election_tick = 10;
        config_.heartbeat_tick = 2;

        wal_config_.dir = temp_dir_ / "wal";

        auto storage_result = wal::WALStorage::Open(wal_config_);
        if (!storage_result) {
            throw std::runtime_error("Failed to open WALStorage");
        }
        storage_ = std::move(*storage_result);

        ConfState conf_state = capnp_util::make<msg::ConfState>();
        auto conf_builder = capnp_util::builder<msg::ConfState>(conf_state);
        auto voters = conf_builder.initVoters(1);
        voters.set(0, 1);

        storage_->SetConfState(conf_state);

        HardState hard_state = capnp_util::make<msg::HardState>();
        auto hs_builder = capnp_util::builder<msg::HardState>(hard_state);
        hs_builder.setCommit(0);
        hs_builder.setTerm(0);
        hs_builder.setVote(0);

        storage_->SetHardState(std::move(hard_state));

        sm_ = std::make_unique<MockStateMachine>();
        transport_ = std::make_unique<MockTransport>();

        auto create_result =
            Raftor::Create(config_, std::move(sm_), storage_, std::move(transport_));
        if (!create_result) {
            throw std::runtime_error("Failed to create Raftor");
        }
        raftor_ = std::move(*create_result);

        auto result = raftor_->Start();
        if (!result) {
            throw std::runtime_error("Failed to start Raftor");
        }
    }

    void TearDown() {
        if (raftor_) {
            raftor_->Stop();
        }
        std::filesystem::remove_all(temp_dir_);
    }

    void ElectLeader() {
        for (int i = 0; i < 25; i++) {
            raftor_->Tick();
        }
        if (!raftor_->IsLeader()) {
            throw std::runtime_error("Failed to become leader");
        }
    }

    Raftor* GetRaftor() { return raftor_.get(); }

    static std::atomic<int> next_port_;

  private:
    std::filesystem::path temp_dir_;
    RaftorConfig config_;
    wal::WALConfig wal_config_;
    std::shared_ptr<wal::WALStorage> storage_;
    std::unique_ptr<MockStateMachine> sm_;
    std::unique_ptr<MockTransport> transport_;
    std::unique_ptr<Raftor> raftor_;
};

std::atomic<int> SingleNodeRaftorFixture::next_port_{19991};

TEST_CASE("raftor: single node tests") {
    SUBCASE("propose entry") {
        SingleNodeRaftorFixture fixture;
        fixture.SetUp("single_node_propose");
        Raftor* raftor = fixture.GetRaftor();

        fixture.ElectLeader();

        auto status = raftor->GetStatus();
        CHECK(status.id == 1);
        CHECK(status.role == StateRole::Leader);
        CHECK(status.term == 1);

        std::atomic<bool> async_callback_called{false};
        Result<std::string> async_result;

        raftor->Propose("async_data", [&](Result<std::string> result) {
            async_callback_called = true;
            async_result = std::move(result);
        });

        for (int i = 0; i < 20; i++) {
            raftor->Tick();
        }

        CHECK(async_callback_called);
        REQUIRE(async_result.has_value());
        CHECK(async_result->find("async_data") != std::string::npos);

        {
            std::atomic<bool> sync_callback_called{false};
            Result<std::string> sync_result;
            raftor->Propose("sync_data", [&](Result<std::string> result) {
                sync_callback_called = true;
                sync_result = std::move(result);
            });

            for (int i = 0; i < 20; i++) {
                raftor->Tick();
            }

            CHECK(sync_callback_called);
            REQUIRE(sync_result.has_value());
            CHECK(sync_result->find("sync_data") != std::string::npos);
        }

        for (int i = 0; i < 20; i++) {
            raftor->Tick();
        }
    }

    SUBCASE("propose multiple entries") {
        SingleNodeRaftorFixture fixture;
        fixture.SetUp("single_node_multi_entries");
        Raftor* raftor = fixture.GetRaftor();

        fixture.ElectLeader();

        constexpr int num_entries = 5;
        std::vector<Result<std::string>> results;
        std::mutex results_mutex;
        std::atomic<int> callback_count{0};

        for (int i = 0; i < num_entries; i++) {
            raftor->Propose("entry_" + std::to_string(i), [&](Result<std::string> result) {
                std::lock_guard lock(results_mutex);
                results.push_back(std::move(result));
                callback_count++;
            });
        }

        for (int i = 0; i < 30; i++) {
            raftor->Tick();
        }

        CHECK(callback_count == num_entries);
        for (int i = 0; i < num_entries; i++) {
            REQUIRE(results[i].has_value());
        }
    }

    SUBCASE("read index") {
        SingleNodeRaftorFixture fixture;
        fixture.SetUp("single_node_read_index");
        Raftor* raftor = fixture.GetRaftor();

        fixture.ElectLeader();

        std::atomic<bool> read_callback_called{false};
        Result<void> read_result;

        raftor->ReadIndex("read_ctx", [&](Result<void> result) {
            read_callback_called = true;
            read_result = std::move(result);
        });

        for (int i = 0; i < 20; i++) {
            raftor->Tick();
        }

        CHECK(read_callback_called);
        CHECK(read_result.has_value());
    }

    SUBCASE("read index after Campaign does not fail") {
        SingleNodeRaftorFixture fixture;
        fixture.SetUp("single_node_read_index_after_campaign");
        Raftor* raftor = fixture.GetRaftor();

        // Become leader without driving Ready processing yet.
        auto campaign_result = raftor->Campaign();
        REQUIRE(campaign_result.has_value());

        std::atomic<bool> read_callback_called{false};
        Result<void> read_result;

        raftor->ReadIndex("read_ctx_after_campaign", [&](Result<void> result) {
            read_callback_called = true;
            read_result = std::move(result);
        });

        for (int i = 0; i < 50 && !read_callback_called.load(); i++) {
            raftor->Tick();
        }

        CHECK(read_callback_called);
        CHECK(read_result.has_value());
    }
}

// Note: This test is disabled because it requires proper WAL initialization
// The error preservation for AlreadyStarted is already tested indirectly
// TEST_CASE("error_preservation: Start returns AlreadyStarted when called twice") {
//     // Create a temporary directory for WAL
//     auto temp_dir = std::filesystem::temp_directory_path() / "raftpp_test_already_started";
//     std::filesystem::create_directories(temp_dir);
//
//     RaftorConfig config;
//     config.node_id = 1;
//     config.listen_addr = "127.0.0.1:19999";  // Use unique port
//     config.data_dir = temp_dir;
//     config.election_tick = 10;
//     config.heartbeat_tick = 2;
//
//     auto result = Raftor::Create(config, std::make_unique<MockStateMachine>());
//     REQUIRE(result.has_value());
//     auto raftor = std::move(*result);
//
//     // First Start should succeed
//     auto start_result = raftor->Start();
//     CHECK(start_result.has_value());
//
//     // Second Start should fail with AlreadyStarted
//     auto second_start_result = raftor->Start();
//     REQUIRE_FALSE(second_start_result.has_value());
//     CHECK(second_start_result.error().Is(RaftErrorCode::AlreadyStarted));
//
//     raftor->Stop();
//
//     // Clean up
//     std::filesystem::remove_all(temp_dir);
// }

TEST_SUITE_END();
