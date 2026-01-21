#include "kv_store_state_machine.h"

#include <nlohmann/json.hpp>

#include "raftpp/core/capnp_util.h"

namespace kvstore {

namespace {

std::string serializeToJson(const Command& cmd) {
    nlohmann::json j;
    j["op"] = [&] {
        switch (cmd.op) {
            case Op::Put:
                return "put";
            case Op::Get:
                return "get";
            case Op::Del:
                return "del";
        }
        return "unknown";
    }();
    j["key"] = cmd.key;
    if (cmd.value.has_value()) {
        j["value"] = cmd.value.value();
    }
    return j.dump();
}

std::optional<Command> parseFromJson(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        Command cmd;
        auto op_str = j.at("op").get<std::string>();
        if (op_str == "put") {
            cmd.op = Op::Put;
        } else if (op_str == "get") {
            cmd.op = Op::Get;
        } else if (op_str == "del") {
            cmd.op = Op::Del;
        } else {
            return std::nullopt;
        }
        cmd.key = j.at("key").get<std::string>();
        if (j.contains("value")) {
            cmd.value = j.at("value").get<std::string>();
        }
        return std::make_optional(cmd);
    } catch (const nlohmann::json::exception& e) {
        return std::nullopt;
    }
}

std::string serializeData(const std::map<std::string, std::string>& data) {
    nlohmann::json j = data;
    return j.dump();
}

std::map<std::string, std::string> deserializeData(const std::string& data_str) {
    try {
        auto j = nlohmann::json::parse(data_str);
        return j.get<std::map<std::string, std::string>>();
    } catch (...) {
        return {};
    }
}

}  // namespace

raftpp::Result<raftpp::raftor::ApplyResult> KvStoreStateMachine::Apply(const raftpp::Entry& entry) {
    auto reader = raftpp::capnp_util::reader<raftpp::msg::Entry>(entry);
    auto data = reader.getData();
    std::string data_str(reinterpret_cast<const char*>(data.begin()), data.size());

    auto cmd_opt = parseFromJson(data_str);
    if (!cmd_opt.has_value()) {
        return std::unexpected(raftpp::RaftError(raftpp::RaftErrorCode::ProposalDropped));
    }

    auto& cmd = *cmd_opt;
    raftpp::raftor::ApplyResult result;

    switch (cmd.op) {
        case Op::Put: {
            std::lock_guard lock(mutex_);
            if (!cmd.value.has_value()) {
                nlohmann::json resp;
                resp["success"] = false;
                resp["error"] = "missing value for put operation";
                result.response = resp.dump();
                break;
            }
            data_[cmd.key] = *cmd.value;
            nlohmann::json resp;
            resp["success"] = true;
            result.response = resp.dump();
            break;
        }
        case Op::Get: {
            std::lock_guard lock(mutex_);
            nlohmann::json resp;
            auto it = data_.find(cmd.key);
            if (it != data_.end()) {
                resp["success"] = true;
                resp["value"] = it->second;
            } else {
                resp["success"] = false;
                resp["error"] = "key not found";
            }
            result.response = resp.dump();
            break;
        }
        case Op::Del: {
            std::lock_guard lock(mutex_);
            nlohmann::json resp;
            resp["success"] = data_.erase(cmd.key) > 0;
            result.response = resp.dump();
            break;
        }
    }

    return result;
}

raftpp::Result<raftpp::raftor::SnapshotData> KvStoreStateMachine::TakeSnapshot(
    uint64_t applied_index, uint64_t applied_term, const raftpp::ConfState& conf_state
) {
    std::lock_guard lock(mutex_);
    raftpp::raftor::SnapshotData snapshot;
    std::string data_str = serializeData(data_);
    snapshot.data = std::vector<uint8_t>(data_str.begin(), data_str.end());
    snapshot.metadata = raftpp::capnp_util::make<raftpp::msg::SnapshotMetadata>();
    auto meta_builder =
        raftpp::capnp_util::builder<raftpp::msg::SnapshotMetadata>(snapshot.metadata);
    meta_builder.setIndex(applied_index);
    meta_builder.setTerm(applied_term);
    meta_builder.setConfState(raftpp::capnp_util::reader<raftpp::msg::ConfState>(conf_state));
    return snapshot;
}

raftpp::Result<void> KvStoreStateMachine::RestoreSnapshot(
    const raftpp::raftor::SnapshotData& snapshot
) {
    std::string data_str(snapshot.data.begin(), snapshot.data.end());
    std::lock_guard lock(mutex_);
    data_ = deserializeData(data_str);
    return {};
}

}  // namespace kvstore
