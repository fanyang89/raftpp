#include "kv_store_state_machine.h"

#include <array>

#include <nlohmann/json.hpp>

#include "raftpp/core/capnp_util.h"

namespace kvstore {

namespace {

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

raftpp::Result<std::map<std::string, std::string>> deserializeData(const std::string& data_str) {
    try {
        auto j = nlohmann::json::parse(data_str);
        return j.get<std::map<std::string, std::string>>();
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(
            raftpp::RaftError(
                raftpp::StorageErrorOther{
                    std::string("kvstore snapshot parse failed: ") + e.what(),
                }
            )
        );
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

raftpp::Result<raftpp::SnapshotMetadata> KvStoreStateMachine::TakeSnapshot(
    uint64_t applied_index, uint64_t applied_term, const raftpp::ConfState& conf_state,
    raftpp::raftor::SnapshotWriter& writer
) {
    std::lock_guard lock(mutex_);
    std::string data_str = serializeData(data_);
    auto write_result = writer.Write(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(data_str.data()), data_str.size()
    ));
    if (!write_result) {
        return std::unexpected(write_result.error());
    }

    auto metadata = raftpp::capnp_util::make<raftpp::msg::SnapshotMetadata>();
    auto meta_builder = raftpp::capnp_util::builder<raftpp::msg::SnapshotMetadata>(metadata);
    meta_builder.setIndex(applied_index);
    meta_builder.setTerm(applied_term);
    meta_builder.setConfState(raftpp::capnp_util::reader<raftpp::msg::ConfState>(conf_state));
    return metadata;
}

raftpp::Result<void> KvStoreStateMachine::RestoreSnapshot(
    const raftpp::SnapshotMetadata& metadata, raftpp::raftor::SnapshotReader& reader
) {
    (void)metadata;
    std::string data_str;
    std::array<uint8_t, 4096> buffer{};
    while (true) {
        auto read_result = reader.Read(buffer);
        if (!read_result) {
            return std::unexpected(read_result.error());
        }
        const size_t bytes_read = *read_result;
        if (bytes_read == 0) {
            break;
        }
        data_str.append(reinterpret_cast<const char*>(buffer.data()), bytes_read);
    }

    auto data_result = deserializeData(data_str);
    if (!data_result) {
        return std::unexpected(data_result.error());
    }

    std::lock_guard lock(mutex_);
    data_ = std::move(*data_result);
    return {};
}

}  // namespace kvstore
