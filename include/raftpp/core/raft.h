#pragma once

#include "progress_tracker.h"
#include "raft_config.h"
#include "raft_core.h"
#include "read_only.h"
#include "types.h"

namespace raftpp {

class Raft : public RaftCore {
  public:
    Raft(const Config& config, const std::shared_ptr<Storage>& store);

    [[nodiscard]] ConfState PostConfChange();
    [[nodiscard]] Result<ConfState> ApplyConfChange(const ConfChangeV2& cc);

    [[nodiscard]] Result<void> Step(Message& m);
    [[nodiscard]] Result<void> StepCandidate(const Message& m);
    [[nodiscard]] Result<void> StepFollower(Message& m);
    [[nodiscard]] Result<void> StepLeader(const Message& m);

    [[nodiscard]] VoteResult Poll(uint64_t from, MessageType mt, bool vote);
    [[nodiscard]] bool AppendEntry(const Entry& entry);
    [[nodiscard]] bool AppendEntry(std::vector<Entry> entries);
    [[nodiscard]] bool CheckQuorumActive();
    [[nodiscard]] bool CommitToCurrentTerm() const;
    [[nodiscard]] bool HasPendingConf() const;
    [[nodiscard]] bool Restore(const Snapshot& snapshot);
    [[nodiscard]] bool ShouldBroadcastCommit() const;
    [[nodiscard]] bool Tick();
    [[nodiscard]] bool TickElection();
    [[nodiscard]] bool TickHeartbeat();

    void BecomeCandidate();
    void BecomeFollower(uint64_t term, uint64_t leader_id);
    void BecomeLeader();
    void BecomePreCandidate();

    void BroadcastAppend();
    void BroadcastHeartbeat();
    void BroadcastHeartbeat(const std::optional<std::string>& ctx);

    void Campaign(std::string_view campaign_type);

    void HandleAppendEntries(const Message& m);
    void HandleAppendResponse(const Message& m);
    void HandleHeartbeat(const Message& m);
    void HandleHeartbeatResponse(const Message& m);
    void HandleSnapshot(const Message& m);
    void HandleSnapshotStatus(const Message& m);
    void HandleTransferLeader(const Message& m);
    void HandleUnreachable(const Message& m);
    [[nodiscard]] std::optional<Message> HandleReadyReadIndex(const Message& req, uint64_t index);

    void Hup(bool transfer_leader);
    void LoadState(const HardState& hs);
    void OnPersistEntries(uint64_t index, uint64_t term);
    void OnPersistSnapshot(uint64_t index);
    void Ping();

    [[nodiscard]] bool MaybeCommit();
    [[nodiscard]] bool MaybeIncreaseUncommittedSize(std::span<const Entry> entries);
    void MaybeCommitByVote(const Message& m);

    void SendAppend(uint64_t to);
    void SendAppendAggressively(uint64_t to);
    void SendHeartbeat(
        uint64_t to, const Progress& pr, const std::optional<std::string>& ctx,
        std::vector<Message>& messages
    );
    void SendRequestSnapshot();
    void SendTimeoutNow(uint64_t to);
    void SetPriority(uint64_t priority);
    void ReduceUncommittedSize(const std::vector<Entry>& ents);
    void CommitApply(uint64_t applied);
    [[nodiscard]] Result<void> RequestSnapshot();

    // Group commit API
    void EnableGroupCommit(bool enable);
    [[nodiscard]] bool GroupCommit() const;
    void AssignCommitGroups(const std::vector<std::pair<uint64_t, uint64_t>>& ids);
    [[nodiscard]] std::optional<bool> CheckGroupCommitConsistent();

    [[nodiscard]] ProgressTracker& progress_tracker();
    [[nodiscard]] const ProgressTracker& progress_tracker() const;
    [[nodiscard]] HardState hard_state() const;
    [[nodiscard]] SoftState soft_state() const;
    [[nodiscard]] const std::vector<ReadState>& read_states() const;
    [[nodiscard]] std::vector<ReadState>& read_states();
    [[nodiscard]] uint64_t id() const;
    [[nodiscard]] uint64_t term() const;
    [[nodiscard]] StateRole state() const;
    [[nodiscard]] const RaftLog& raft_log() const;
    [[nodiscard]] RaftLog& raft_log();
    [[nodiscard]] uint64_t max_committed_size_per_ready() const;
    [[nodiscard]] uint64_t& max_committed_size_per_ready();
    [[nodiscard]] size_t max_inflight_messages() const;
    [[nodiscard]] size_t inflight_buffers_size() const;
    void MaybeFreeInflightBuffers();
    void AdjustMaxInflightMsgs(uint64_t id, size_t max_inflight);
    [[nodiscard]] static bool ConfStatesEqualIgnoringOrder(const ConfState& a, const ConfState& b);
    [[nodiscard]] const std::vector<Message>& messages() const;
    [[nodiscard]] std::vector<Message>& messages();
    [[nodiscard]] std::optional<std::reference_wrapper<Snapshot>> snapshot();
    [[nodiscard]] const std::optional<Snapshot>& snapshot() const;

  private:
    bool HasUnappliedConfChanges(uint64_t low, uint64_t high, const GetEntriesContext& ctx);
    void AbortLeaderTransfer();
    void CommitApplyInternal(uint64_t applied, bool skip_check);
    void Reset(uint64_t term);
    void ResetRandomizedElectionTimeout();

    static MessageType VoteRespMsgType(MessageType mt);

    ProgressTracker progress_tracker_;
    std::vector<Message> messages_;
    Config config_;
};

}  // namespace raftpp
