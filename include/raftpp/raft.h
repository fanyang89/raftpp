#pragma once

#include "raftpp/config.h"
#include "raftpp/raft_core.h"
#include "raftpp/raftpp.pb.h"
#include "raftpp/readonly.h"
#include "raftpp/tracker.h"

namespace raftpp {

class Raft : public RaftCore {
public:
    Raft(const Config& config, std::unique_ptr<Storage> store);

    ConfState PostConfChange();
    void LoadState(const HardState& hs);
    bool MaybeIncreaseUncommittedSize(std::span<const Entry> entries);
    bool AppendEntry(Entry& entry);
    bool AppendEntry(std::span<Entry> entries);
    bool MaybeCommit();
    bool ShouldBroadcastCommit() const;
    bool HasPendingConf() const;
    void BroadcastAppend();

    void OnPersistEntries(uint64_t index, uint64_t term);
    void OnPersistSnapshot(uint64_t index);

    void BecomePreCandidate();
    void BecomeCandidate();
    void BecomeLeader();

    VoteResult Poll(uint64_t from, MessageType mt, bool vote);
    void Campaign(std::string_view campaign_type);
    void Hup(bool transfer_leader);
    void MaybeCommitByVote(const Message& m);

    void SendTimeoutNow(uint64_t to);
    void HandleAppendResponse(const Message& m);
    void SendRequestSnapshot();
    void HandleHeartbeat(const Message& m);
    void HandleSnapshot(const Message& m);
    std::optional<Message> HandleReadyReadIndex(const Message& req, uint64_t index);
    bool Restore(const Snapshot& snapshot);

    Result<void> Step(Message& m);
    void HandleAppendEntries(const Message& m);
    Result<void> StepCandidate(const Message& m);
    Result<void> StepFollower(Message& m);
    bool CheckQuorumActive();
    void HandleHeartbeatResponse(const Message& m);
    void HandleSnapshotStatus(const Message& m);
    void HandleUnreachable(const Message& m);
    void HandleTransferLeader(const Message& m);
    Result<void> StepLeader(const Message& m);
    void SendAppend(uint64_t to);
    void SendAppendAggressively(uint64_t to);

    void BroadcastHeartbeat();
    void SendHeartbeat(
        uint64_t to, const Progress& pr, const std::optional<std::string>& ctx, std::vector<Message>& messages
    );
    void BroadcastHeartbeat(const std::optional<std::string>& ctx);

    bool TickElection();
    bool TickHeartbeat();
    bool Tick();

    ProgressTracker& progress_tracker();
    const ProgressTracker& progress_tracker() const;

private:
    bool HasUnappliedConfChanges(uint64_t low, uint64_t high, const GetEntriesContext& ctx);
    void CommitApplyInternal(uint64_t applied, bool skip_check);
    void AbortLeaderTransfer();
    void Reset(uint64_t term);
    void BecomeFollower(uint64_t term, uint64_t leader_id);
    void ResetRandomizedElectionTimeout();

    static MessageType VoteRespMsgType(MessageType mt);

    ProgressTracker progress_tracker_;
    std::vector<Message> messages_;
    Config config_;
};

} // namespace raftpp
