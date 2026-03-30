#include <array>
#include <chrono>

#include "raftpp/core/capnp_util.h"
#include "raftpp/raftor/raftor.h"

namespace {

using namespace std::chrono_literals;

class MinimalStateMachine final : public raftpp::raftor::StateMachine {
  public:
    raftpp::Result<raftpp::raftor::ApplyResult> Apply(const raftpp::Entry& entry) override {
        (void)entry;
        return raftpp::raftor::ApplyResult{.response = "ok"};
    }

    raftpp::Result<raftpp::SnapshotMetadata> TakeSnapshot(
        uint64_t applied_index, uint64_t applied_term, const raftpp::ConfState& conf_state,
        raftpp::raftor::SnapshotWriter& writer
    ) override {
        const std::array<uint8_t, 1> payload = {'x'};
        if (auto result = writer.Write(payload); !result) {
            return nonstd::make_unexpected(result.error());
        }

        auto metadata = raftpp::capnp_util::make<raftpp::msg::SnapshotMetadata>();
        auto meta = raftpp::capnp_util::builder<raftpp::msg::SnapshotMetadata>(metadata);
        meta.setIndex(applied_index);
        meta.setTerm(applied_term);
        meta.setConfState(raftpp::capnp_util::reader<raftpp::msg::ConfState>(conf_state));
        return metadata;
    }

    raftpp::Result<void> RestoreSnapshot(
        const raftpp::SnapshotMetadata& metadata, raftpp::raftor::SnapshotReader& reader
    ) override {
        (void)metadata;
        std::array<uint8_t, 256> buffer{};
        while (true) {
            auto result = reader.Read(buffer);
            if (!result) {
                return nonstd::make_unexpected(result.error());
            }
            if (*result == 0) {
                return {};
            }
        }
    }
};

}  // namespace

int main() {
    raftpp::raftor::RaftorConfig config;
    config.node_id = 1;
    config.listen_addr = "127.0.0.1:9001";
    config.data_dir = "./minimal-node-data";
    config.tick_interval = 100ms;

    auto raftor_result =
        raftpp::raftor::Raftor::Create(config, std::make_unique<MinimalStateMachine>());
    if (!raftor_result) {
        return 1;
    }

    auto raftor = std::move(*raftor_result);
    if (auto result = raftor->Start(); !result) {
        return 1;
    }

    for (int i = 0; i < 20; ++i) {
        raftor->Poll(config.tick_interval);
        if (raftor->GetStatus().role == raftpp::StateRole::Leader) {
            raftor->Stop();
            return 0;
        }
    }

    raftor->Stop();
    return 1;
}
