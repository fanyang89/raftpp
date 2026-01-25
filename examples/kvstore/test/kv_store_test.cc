#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "kv_store_state_machine.h"
#include "raftpp/core/capnp_util.h"
#include "raftpp/core/types.h"

using namespace kvstore;

namespace {

raftpp::Entry CreateTestEntry(const std::string& data) {
    auto entry = raftpp::capnp_util::make<raftpp::msg::Entry>();
    auto builder = raftpp::capnp_util::builder<raftpp::msg::Entry>(entry);
    builder.setData(kj::arrayPtr(reinterpret_cast<const kj::byte*>(data.data()), data.size()));
    return entry;
}

raftpp::ConfState CreateTestConfState() {
    auto conf_state = raftpp::capnp_util::make<raftpp::msg::ConfState>();
    auto builder = raftpp::capnp_util::builder<raftpp::msg::ConfState>(conf_state);
    auto voters = builder.initVoters(1);
    voters.set(0, 1);
    return conf_state;
}

}  // namespace

TEST_SUITE_BEGIN("kv_store");

TEST_CASE("kv_store: put and get") {
    KvStoreStateMachine sm;

    auto put_json = nlohmann::json{{"op", "put"}, {"key", "key1"}, {"value", "value1"}}.dump();

    auto result = sm.Apply(CreateTestEntry(put_json));
    CHECK(result.has_value());
    CHECK(result->response.has_value());
    auto resp = nlohmann::json::parse(*result->response);
    CHECK(resp["success"] == true);

    auto get_result = sm.Get("key1");
    CHECK(get_result.has_value());
    CHECK(*get_result == "value1");
}

TEST_CASE("kv_store: get nonexistent key") {
    KvStoreStateMachine sm;

    auto get_json = nlohmann::json{{"op", "get"}, {"key", "nonexistent"}}.dump();
    auto result = sm.Apply(CreateTestEntry(get_json));
    CHECK(result.has_value());
    CHECK(result->response.has_value());
    auto resp = nlohmann::json::parse(*result->response);
    CHECK(resp["success"] == false);
    CHECK(resp["error"] == "key not found");
}

TEST_CASE("kv_store: delete key") {
    KvStoreStateMachine sm;

    auto put_json = nlohmann::json{{"op", "put"}, {"key", "key2"}, {"value", "value2"}}.dump();
    auto put_result = sm.Apply(CreateTestEntry(put_json));
    CHECK(put_result.has_value());

    auto del_json = nlohmann::json{{"op", "del"}, {"key", "key2"}}.dump();
    auto del_result = sm.Apply(CreateTestEntry(del_json));
    CHECK(del_result.has_value());
    auto del_resp = nlohmann::json::parse(*del_result->response);
    CHECK(del_resp["success"] == true);

    auto get_result = sm.Get("key2");
    CHECK(!get_result.has_value());
}

TEST_CASE("kv_store: update existing key") {
    KvStoreStateMachine sm;

    auto put1_json = nlohmann::json{{"op", "put"}, {"key", "key3"}, {"value", "value1"}}.dump();
    auto put1_result = sm.Apply(CreateTestEntry(put1_json));
    CHECK(put1_result.has_value());

    auto put2_json = nlohmann::json{{"op", "put"}, {"key", "key3"}, {"value", "value2"}}.dump();
    auto put2_result = sm.Apply(CreateTestEntry(put2_json));
    CHECK(put2_result.has_value());

    auto get_result = sm.Get("key3");
    CHECK(get_result.has_value());
    CHECK(*get_result == "value2");
}

TEST_CASE("kv_store: snapshot and restore") {
    KvStoreStateMachine sm;

    auto put1_json =
        nlohmann::json{{"op", "put"}, {"key", "snap_key1"}, {"value", "snap_value1"}}.dump();
    auto put1_result = sm.Apply(CreateTestEntry(put1_json));
    CHECK(put1_result.has_value());

    auto put2_json =
        nlohmann::json{{"op", "put"}, {"key", "snap_key2"}, {"value", "snap_value2"}}.dump();
    auto put2_result = sm.Apply(CreateTestEntry(put2_json));
    CHECK(put2_result.has_value());

    auto snapshot_result = sm.TakeSnapshot(2, 1, CreateTestConfState());
    CHECK(snapshot_result.has_value());

    KvStoreStateMachine sm2;
    auto restore_result = sm2.RestoreSnapshot(*snapshot_result);
    CHECK(restore_result.has_value());

    CHECK(sm2.Get("snap_key1").has_value());
    CHECK(*sm2.Get("snap_key1") == "snap_value1");
    CHECK(sm2.Get("snap_key2").has_value());
    CHECK(*sm2.Get("snap_key2") == "snap_value2");
}

TEST_CASE("kv_store: restore corrupted snapshot fails and preserves state") {
    KvStoreStateMachine sm;

    auto put1_json =
        nlohmann::json{{"op", "put"}, {"key", "snap_key1"}, {"value", "snap_value1"}}.dump();
    auto put1_result = sm.Apply(CreateTestEntry(put1_json));
    CHECK(put1_result.has_value());

    auto put2_json =
        nlohmann::json{{"op", "put"}, {"key", "snap_key2"}, {"value", "snap_value2"}}.dump();
    auto put2_result = sm.Apply(CreateTestEntry(put2_json));
    CHECK(put2_result.has_value());

    auto snapshot_result = sm.TakeSnapshot(2, 1, CreateTestConfState());
    CHECK(snapshot_result.has_value());

    snapshot_result->data = std::vector<uint8_t>{'{'};

    KvStoreStateMachine sm2;

    auto pre_put1_json =
        nlohmann::json{{"op", "put"}, {"key", "snap_key1"}, {"value", "local_value"}}.dump();
    auto pre_put1_result = sm2.Apply(CreateTestEntry(pre_put1_json));
    CHECK(pre_put1_result.has_value());

    auto pre_put2_json =
        nlohmann::json{{"op", "put"}, {"key", "pre_key"}, {"value", "pre_value"}}.dump();
    auto pre_put2_result = sm2.Apply(CreateTestEntry(pre_put2_json));
    CHECK(pre_put2_result.has_value());

    CHECK(sm2.Get("snap_key1").has_value());
    CHECK(*sm2.Get("snap_key1") == "local_value");
    CHECK(sm2.Get("pre_key").has_value());
    CHECK(*sm2.Get("pre_key") == "pre_value");

    auto restore_result = sm2.RestoreSnapshot(*snapshot_result);
    CHECK(!restore_result.has_value());

    CHECK(sm2.Get("snap_key1").has_value());
    CHECK(*sm2.Get("snap_key1") == "local_value");
    CHECK(sm2.Get("pre_key").has_value());
    CHECK(*sm2.Get("pre_key") == "pre_value");
    CHECK(!sm2.Get("snap_key2").has_value());
}

TEST_SUITE_END();
