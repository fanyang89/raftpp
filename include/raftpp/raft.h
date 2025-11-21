#pragma once

#include "raftpp/progress_tracker.h"
#include "raftpp/raft_config.h"
#include "raftpp/raft_core.h"
#include "raftpp/raftpp.pb.h"
#include "raftpp/read_only.h"

namespace raftpp {

class Raft : public RaftCore {
  public:
    Raft(const Config& config, std::unique_ptr<Storage> store);

    ConfState PostConfChange();
    Result<ConfState> ApplyConfChange(const ConfChangeV2& cc);

    Result<void> Step(Message& m);
    Result<void> StepCandidate(const Message& m);
    Result<void> StepFollower(Message& m);
    Result<void> StepLeader(const Message& m);

    VoteResult Poll(uint64_t from, MessageType mt, bool vote);
    bool AppendEntry(Entry& entry);
    bool AppendEntry(std::span<Entry> entries);
    bool CheckQuorumActive();
    bool HasPendingConf() const;
    bool Restore(const Snapshot& snapshot);
    bool ShouldBroadcastCommit() const;
    bool Tick();
    bool TickElection();
    bool TickHeartbeat();

    void BecomeCandidate();
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
    std::optional<Message> HandleReadyReadIndex(const Message& req, uint64_t index);

    void Hup(bool transfer_leader);
    void LoadState(const HardState& hs);
    void OnPersistEntries(uint64_t index, uint64_t term);
    void OnPersistSnapshot(uint64_t index);
    void Ping();

    bool MaybeCommit();
    bool MaybeIncreaseUncommittedSize(std::span<const Entry> entries);
    void MaybeCommitByVote(const Message& m);

    void SendAppend(uint64_t to);
    void SendAppendAggressively(uint64_t to);
    void SendHeartbeat(
        uint64_t to, const Progress& pr, const std::optional<std::string>& ctx, std::vector<Message>& messages
    );
    void SendRequestSnapshot();
    void SendTimeoutNow(uint64_t to);
    void SetPriority(uint64_t priority);
    void ReduceUncommittedSize(const std::vector<Entry>& ents);
    void CommitApply(uint64_t applied);
    Result<void> RequestSnapshot();

    ProgressTracker& progress_tracker();
    const ProgressTracker& progress_tracker() const;
    HardState hard_state() const;
    SoftState soft_state() const;
    const std::vector<ReadState>& read_states() const;
    std::vector<ReadState>& read_states();
    uint64_t id() const;
    uint64_t term() const;
    StateRole state() const;
    const RaftLog& raft_log() const;
    RaftLog& raft_log();
    uint64_t max_committed_size_per_ready() const;
    uint64_t& max_committed_size_per_ready();
    const std::vector<Message>& messages() const;
    std::vector<Message>& messages();
    std::optional<std::reference_wrapper<Snapshot>> snapshot();
    const std::optional<Snapshot>& snapshot() const;

  private:
    bool HasUnappliedConfChanges(uint64_t low, uint64_t high, const GetEntriesContext& ctx);
    void AbortLeaderTransfer();
    void BecomeFollower(uint64_t term, uint64_t leader_id);
    void CommitApplyInternal(uint64_t applied, bool skip_check);
    void Reset(uint64_t term);
    void ResetRandomizedElectionTimeout();

    static MessageType VoteRespMsgType(MessageType mt);

    ProgressTracker progress_tracker_;
    std::vector<Message> messages_;
    Config config_;
};

}  // namespace raftpp
