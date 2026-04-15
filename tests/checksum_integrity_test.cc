#include <chrono>
#include <filesystem>
#include <thread>

#include <doctest/doctest.h>

#include "raftpp/core/capnp_util.h"
#include "raftpp/core/raw_node.h"
#include "raftpp/raftor/entry_checksum.h"
#include "raftpp/raftor/mock_state_machine.h"
#include "raftpp/raftor/raftor.h"

using namespace raftpp;
using namespace raftpp::raftor;
using namespace std::chrono_literals;

TEST_CASE("raftor: end-to-end data integrity checksum") {
    uint64_t node_id = 1;
    std::filesystem::path temp_dir = "temp_checksum_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    RaftorConfig config;
    config.node_id = node_id;
    config.data_dir = temp_dir;
    config.listen_addr = "127.0.0.1:23456";
    config.initial_peers = {{node_id, "127.0.0.1:23456"}};
    config.tick_interval = 10ms;
    config.election_tick = 5;
    config.enable_entry_checksum = true;

    auto sm_ptr = std::make_unique<MockStateMachine>();
    auto* sm = sm_ptr.get();
    auto raftor_res = Raftor::Create(config, std::move(sm_ptr));
    REQUIRE(raftor_res.has_value());
    auto& raftor = *raftor_res;
    REQUIRE(raftor->Start().has_value());

    // Wait for leader election
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!raftor->IsLeader() && std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(5ms);
    }
    REQUIRE(raftor->IsLeader());

    SUBCASE("successful proposal with valid checksum") {
        std::string data = "integrity check data";

        std::promise<Result<std::string>> promise;
        auto future = promise.get_future();
        raftor->Propose(data, [&](Result<std::string> res) { promise.set_value(res); });

        deadline = std::chrono::steady_clock::now() + 2s;
        while (future.wait_for(0ms) != std::future_status::ready &&
               std::chrono::steady_clock::now() < deadline) {
            raftor->Poll(5ms);
        }

        REQUIRE(future.wait_for(0ms) == std::future_status::ready);
        auto result = future.get();
        if (!result.has_value()) {
            FAIL(result.error().ToString());
        }
        CHECK(result.value() == "OK:" + data);
    }

    SUBCASE("successful proposal with empty payload") {
        std::promise<Result<std::string>> promise;
        auto future = promise.get_future();
        raftor->Propose("", [&](Result<std::string> res) { promise.set_value(res); });

        deadline = std::chrono::steady_clock::now() + 2s;
        while (future.wait_for(0ms) != std::future_status::ready &&
               std::chrono::steady_clock::now() < deadline) {
            raftor->Poll(5ms);
        }

        REQUIRE(future.wait_for(0ms) == std::future_status::ready);
        auto result = future.get();
        if (!result.has_value()) {
            FAIL(result.error().ToString());
        }
        CHECK(result.value() == "OK:");
    }

    SUBCASE("detected corruption via manual message step") {
        auto& raw_node = raftor->GetRawNode();
        const uint64_t initial_applied_index = raw_node.GetStatus().applied;

        Message m = capnp_util::make<msg::Message>();
        auto m_builder = capnp_util::builder<msg::Message>(m);
        m_builder.setMsgType(capnp_util::cast_enum<msg::MessageType>(MessageType::MSG_PROPOSE));
        m_builder.setFrom(node_id);

        auto entries = m_builder.initEntries(1);
        auto entry_builder = entries[0];
        std::string data = "corrupt data";
        entry_builder.setData(
            kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size())
        );
        entry_builder.setContext(kj::arrayPtr(reinterpret_cast<const kj::byte*>("ctx"), 3));

        // Set a wrong checksum.
        entry_builder.setChecksum(0xDEADBEEF);

        // Move the message into Step
        auto step_res = raw_node.Step(std::move(m));
        REQUIRE(step_res.has_value());
        REQUIRE(raw_node.HasReady());

        size_t initial_apply_count = sm->ApplyCount();

        // Poll until the pending Ready is consumed.
        deadline = std::chrono::steady_clock::now() + 2s;
        while (raw_node.HasReady() && std::chrono::steady_clock::now() < deadline) {
            raftor->Poll(5ms);
        }
        CHECK_FALSE(raw_node.HasReady());

        // It should NOT have been applied
        CHECK(sm->ApplyCount() == initial_apply_count);
        CHECK(raw_node.GetStatus().applied == initial_applied_index);
        CHECK_FALSE(raftor->IsRunning());

        auto proposal_result = raftor->ProposeSync("after corruption", 50ms);
        CHECK_FALSE(proposal_result.has_value());
        CHECK(proposal_result.error() == RaftErrorCode::ChecksumMismatch);

        auto read_result = raftor->ReadIndexSync("after corruption", 50ms);
        CHECK_FALSE(read_result.has_value());
        CHECK(read_result.error() == RaftErrorCode::ChecksumMismatch);
    }

    SUBCASE("detected corruption for conf change before apply") {
        auto& raw_node = raftor->GetRawNode();
        const uint64_t initial_applied_index = raw_node.GetStatus().applied;

        ConfChangeV2 cc = capnp_util::make<msg::ConfChangeV2>();
        auto cc_builder = capnp_util::builder<msg::ConfChangeV2>(cc);
        auto changes = cc_builder.initChanges(1);
        changes[0].setChangeType(ConfChangeType::ADD_NODE);
        changes[0].setNodeId(2);
        cc_builder.setContext(kj::arrayPtr(reinterpret_cast<const kj::byte*>("127.0.0.1:23457"), 15)
        );

        const std::string serialized = capnp_util::toString(cc);

        Message m = capnp_util::make<msg::Message>();
        auto m_builder = capnp_util::builder<msg::Message>(m);
        m_builder.setMsgType(capnp_util::cast_enum<msg::MessageType>(MessageType::MSG_PROPOSE));
        m_builder.setFrom(node_id);

        auto entries = m_builder.initEntries(1);
        auto entry_builder = entries[0];
        entry_builder.setEntryType(
            capnp_util::cast_enum<msg::EntryType>(EntryType::ENTRY_CONF_CHANGE_V2)
        );
        entry_builder.setData(
            kj::arrayPtr(reinterpret_cast<const kj::byte*>(serialized.data()), serialized.size())
        );
        entry_builder.setContext(kj::arrayPtr(reinterpret_cast<const kj::byte*>("ctx-conf"), 8));
        entry_builder.setChecksum(0xDEADBEEF);

        auto step_res = raw_node.Step(std::move(m));
        REQUIRE(step_res.has_value());
        REQUIRE(raw_node.HasReady());

        deadline = std::chrono::steady_clock::now() + 2s;
        while (raw_node.HasReady() && std::chrono::steady_clock::now() < deadline) {
            raftor->Poll(5ms);
        }
        CHECK_FALSE(raw_node.HasReady());

        CHECK(raw_node.GetStatus().applied == initial_applied_index);
        CHECK(raw_node.raft().progress_tracker().get(2) == nullptr);
        CHECK_FALSE(raftor->IsRunning());

        auto proposal_result = raftor->ProposeSync("after conf corruption", 50ms);
        CHECK_FALSE(proposal_result.has_value());
        CHECK(proposal_result.error() == RaftErrorCode::ChecksumMismatch);

        auto read_result = raftor->ReadIndexSync("after conf corruption", 50ms);
        CHECK_FALSE(read_result.has_value());
        CHECK(read_result.error() == RaftErrorCode::ChecksumMismatch);
    }

    SUBCASE("detected corruption when only context changes") {
        auto& raw_node = raftor->GetRawNode();
        const uint64_t initial_applied_index = raw_node.GetStatus().applied;

        Message m = capnp_util::make<msg::Message>();
        auto m_builder = capnp_util::builder<msg::Message>(m);
        m_builder.setMsgType(capnp_util::cast_enum<msg::MessageType>(MessageType::MSG_PROPOSE));
        m_builder.setFrom(node_id);

        auto entries = m_builder.initEntries(1);
        auto entry_builder = entries[0];
        std::string data = "context-protected";
        entry_builder.setData(
            kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size())
        );
        entry_builder.setContext(kj::arrayPtr(reinterpret_cast<const kj::byte*>("ctx-ok"), 6));
        SetEntryChecksum(entry_builder);

        entry_builder.setContext(kj::arrayPtr(reinterpret_cast<const kj::byte*>("ctx-bad"), 7));

        auto step_res = raw_node.Step(std::move(m));
        REQUIRE(step_res.has_value());
        REQUIRE(raw_node.HasReady());

        deadline = std::chrono::steady_clock::now() + 2s;
        while (raw_node.HasReady() && std::chrono::steady_clock::now() < deadline) {
            raftor->Poll(5ms);
        }
        CHECK_FALSE(raw_node.HasReady());

        CHECK(sm->ApplyCount() == 0);
        CHECK(raw_node.GetStatus().applied == initial_applied_index);
        CHECK_FALSE(raftor->IsRunning());
    }

    raftor->Stop();
    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("raftor: legacy entries without checksum remain compatible by default") {
    uint64_t node_id = 1;
    std::filesystem::path temp_dir = "temp_checksum_compat_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    RaftorConfig config;
    config.node_id = node_id;
    config.data_dir = temp_dir;
    config.listen_addr = "127.0.0.1:23458";
    config.initial_peers = {{node_id, "127.0.0.1:23458"}};
    config.tick_interval = 10ms;
    config.election_tick = 5;

    auto sm_ptr = std::make_unique<MockStateMachine>();
    auto* sm = sm_ptr.get();
    auto raftor_res = Raftor::Create(config, std::move(sm_ptr));
    REQUIRE(raftor_res.has_value());
    auto& raftor = *raftor_res;
    REQUIRE(raftor->Start().has_value());

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!raftor->IsLeader() && std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(5ms);
    }
    REQUIRE(raftor->IsLeader());

    auto& raw_node = raftor->GetRawNode();
    Message m = capnp_util::make<msg::Message>();
    auto m_builder = capnp_util::builder<msg::Message>(m);
    m_builder.setMsgType(capnp_util::cast_enum<msg::MessageType>(MessageType::MSG_PROPOSE));
    m_builder.setFrom(node_id);

    auto entries = m_builder.initEntries(1);
    auto entry_builder = entries[0];
    std::string data = "legacy-entry";
    entry_builder.setData(kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size())
    );
    entry_builder.setContext(kj::arrayPtr(reinterpret_cast<const kj::byte*>("legacy-ctx"), 10));

    auto step_res = raw_node.Step(std::move(m));
    REQUIRE(step_res.has_value());
    REQUIRE(raw_node.HasReady());

    deadline = std::chrono::steady_clock::now() + 2s;
    while (raw_node.HasReady() && std::chrono::steady_clock::now() < deadline) {
        raftor->Poll(5ms);
    }

    CHECK_FALSE(raw_node.HasReady());
    CHECK(raftor->IsRunning());
    CHECK(sm->ApplyCount() == 1);

    raftor->Stop();
    std::filesystem::remove_all(temp_dir);
}
