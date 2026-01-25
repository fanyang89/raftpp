#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "kv_store.h"
#include "raftpp/core/types.h"
#include "raftpp/raftor/state_machine.h"

namespace kvstore {

class KvStoreStateMachine : public raftpp::raftor::StateMachine, public IKVStore {
  public:
    KvStoreStateMachine() = default;

    std::optional<std::string> Get(const std::string& key) override {
        std::lock_guard lock(mutex_);
        auto it = data_.find(key);
        if (it != data_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool Put(const std::string& key, const std::string& value) override {
        std::lock_guard lock(mutex_);
        data_[key] = value;
        return true;
    }

    bool Del(const std::string& key) override {
        std::lock_guard lock(mutex_);
        return data_.erase(key) > 0;
    }

    [[nodiscard]] raftpp::Result<raftpp::raftor::ApplyResult> Apply(
        const raftpp::Entry& entry
    ) override;

    [[nodiscard]] raftpp::Result<raftpp::raftor::SnapshotData> TakeSnapshot(
        uint64_t applied_index, uint64_t applied_term, const raftpp::ConfState& conf_state
    ) override;

    [[nodiscard]] raftpp::Result<void> RestoreSnapshot(
        const raftpp::raftor::SnapshotData& snapshot
    ) override;

    void OnLeadershipChange(bool is_leader, uint64_t term, uint64_t leader_id) override {
        (void)is_leader;
        (void)term;
        (void)leader_id;
    }

    void OnPeerUnreachable(uint64_t peer_id) override { (void)peer_id; }

  private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> data_;
};

}  // namespace kvstore
